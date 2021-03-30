# Merge active and previous version's generated next major version candidate
# shadow. This involve simultaneously traversing both FileDescriptorProtos and:
# 1. Recovering hidden_envoy_deprecated_* fields and enum values in active proto.
# 2. Recovering deprecated (sub)message types.
# 3. Misc. fixups for oneof metadata and reserved ranges/names.

from collections import defaultdict
import copy
import pathlib
import sys

from tools.api_proto_plugin import type_context as api_type_context
from tools.protoxform import utils

from google.protobuf import descriptor_pb2, text_format
from envoy.annotations import deprecation_pb2

PROTO_PACKAGES = (
    "google.api.annotations", "validate.validate", "envoy.annotations.deprecation",
    "envoy.annotations.resource", "udpa.annotations.migrate", "udpa.annotations.security",
    "udpa.annotations.status", "udpa.annotations.sensitive", "udpa.annotations.versioning")


# Set reserved_range in target_proto to reflect previous_reserved_range skipping
# skip_reserved_numbers.
def adjust_reserved_range(target_proto, previous_reserved_range, skip_reserved_numbers):
    del target_proto.reserved_range[:]
    for rr in previous_reserved_range:
        # We can only handle singleton ranges today.
        assert ((rr.start == rr.end) or (rr.end == rr.start + 1))
        if rr.start not in skip_reserved_numbers:
            target_proto.reserved_range.add().MergeFrom(rr)


# Add dependencies for envoy.annotations.disallowed_by_default
def add_deprecation_dependencies(target_proto_dependencies, proto_field, is_enum):
    if is_enum:
        if proto_field.options.HasExtension(deprecation_pb2.disallowed_by_default_enum) and \
            "envoy/annotations/deprecation.proto" not in target_proto_dependencies:
            target_proto_dependencies.append("envoy/annotations/deprecation.proto")
    else:
        if proto_field.options.HasExtension(deprecation_pb2.disallowed_by_default) and \
            "envoy/annotations/deprecation.proto" not in target_proto_dependencies:
            target_proto_dependencies.append("envoy/annotations/deprecation.proto")
        if proto_field.type_name == ".google.protobuf.Struct" and \
            "google/protobuf/struct.proto" not in target_proto_dependencies:
            target_proto_dependencies.append("google/protobuf/struct.proto")


# Merge active/shadow EnumDescriptorProtos to a fresh target EnumDescriptorProto.
def merge_active_shadow_enum(active_proto, shadow_proto, target_proto, target_proto_dependencies):
    target_proto.MergeFrom(active_proto)
    if not shadow_proto:
        return
    shadow_values = {v.name: v for v in shadow_proto.value}
    skip_reserved_numbers = []
    # For every reserved name, check to see if it's in the shadow, and if so,
    # reintroduce in target_proto.
    del target_proto.reserved_name[:]
    for n in active_proto.reserved_name:
        hidden_n = 'hidden_envoy_deprecated_' + n
        if hidden_n in shadow_values:
            v = shadow_values[hidden_n]
            add_deprecation_dependencies(target_proto_dependencies, v, True)
            skip_reserved_numbers.append(v.number)
            target_proto.value.add().MergeFrom(v)
        else:
            target_proto.reserved_name.append(n)
    adjust_reserved_range(target_proto, active_proto.reserved_range, skip_reserved_numbers)
    # Special fixup for deprecation of default enum values.
    for tv in target_proto.value:
        if tv.name == 'DEPRECATED_AND_UNAVAILABLE_DO_NOT_USE':
            for sv in shadow_proto.value:
                if sv.number == tv.number:
                    assert (sv.number == 0)
                    tv.CopyFrom(sv)


# Adjust source code info comments path to reflect insertions of oneof fields
# inside the middle of an existing collection of fields.
def adjust_source_code_info(type_context, field_index, field_adjustment):

    def has_path_prefix(s, t):
        return len(s) <= len(t) and all(p[0] == p[1] for p in zip(s, t))

    for loc in type_context.source_code_info.proto.location:
        if has_path_prefix(type_context.path + [2], loc.path):
            path_field_index = len(type_context.path) + 1
            if path_field_index < len(loc.path) and loc.path[path_field_index] >= field_index:
                loc.path[path_field_index] += field_adjustment


# Merge active/shadow DescriptorProtos to a fresh target DescriptorProto.
def merge_active_shadow_message(
        type_context, active_proto, shadow_proto, target_proto, target_proto_dependencies):
    target_proto.MergeFrom(active_proto)
    if not shadow_proto:
        return
    shadow_fields = {f.name: f for f in shadow_proto.field}
    skip_reserved_numbers = []
    # For every reserved name, check to see if it's in the shadow, and if so,
    # reintroduce in target_proto. We track both the normal fields we need to add
    # back in (extra_simple_fields) and those that belong to oneofs
    # (extra_oneof_fields). The latter require special treatment, as we can't just
    # append them to the end of the message, they need to be reordered.
    extra_simple_fields = []
    extra_oneof_fields = defaultdict(list)  # oneof index -> list of fields
    del target_proto.reserved_name[:]
    for n in active_proto.reserved_name:
        hidden_n = 'hidden_envoy_deprecated_' + n
        if hidden_n in shadow_fields:
            f = shadow_fields[hidden_n]
            add_deprecation_dependencies(target_proto_dependencies, f, False)
            skip_reserved_numbers.append(f.number)
            missing_field = copy.deepcopy(f)
            # oneof fields from the shadow need to have their index set to the
            # corresponding index in active/target_proto.
            if missing_field.HasField('oneof_index'):
                oneof_name = shadow_proto.oneof_decl[missing_field.oneof_index].name
                missing_oneof_index = None
                for oneof_index, oneof_decl in enumerate(target_proto.oneof_decl):
                    if oneof_decl.name == oneof_name:
                        missing_oneof_index = oneof_index
                if missing_oneof_index is None:
                    missing_oneof_index = len(target_proto.oneof_decl)
                    target_proto.oneof_decl.add().MergeFrom(
                        shadow_proto.oneof_decl[missing_field.oneof_index])
                missing_field.oneof_index = missing_oneof_index
                extra_oneof_fields[missing_oneof_index].append(missing_field)
            else:
                extra_simple_fields.append(missing_field)
        else:
            target_proto.reserved_name.append(n)
    # Copy existing fields, as we need to nuke them.
    existing_fields = copy.deepcopy(target_proto.field)
    del target_proto.field[:]
    # Rebuild fields, taking into account extra_oneof_fields. protoprint.py
    # expects that oneof fields are consecutive, so need to sort for this.
    current_oneof_index = None

    def append_extra_oneof_fields(current_oneof_index, last_oneof_field_index):
        # Add fields from extra_oneof_fields for current_oneof_index.
        for oneof_f in extra_oneof_fields[current_oneof_index]:
            target_proto.field.add().MergeFrom(oneof_f)
        field_adjustment = len(extra_oneof_fields[current_oneof_index])
        # Fixup the comments in source code info. Note that this is really
        # inefficient, O(N^2) in the worst case, but since we have relatively few
        # deprecated fields, is the easiest to implement method.
        if last_oneof_field_index is not None:
            adjust_source_code_info(type_context, last_oneof_field_index, field_adjustment)
        del extra_oneof_fields[current_oneof_index]
        return field_adjustment

    field_index = 0
    for f in existing_fields:
        if current_oneof_index is not None:
            field_oneof_index = f.oneof_index if f.HasField('oneof_index') else None
            # Are we exiting the oneof? If so, add the respective extra_one_fields.
            if field_oneof_index != current_oneof_index:
                field_index += append_extra_oneof_fields(current_oneof_index, field_index)
                current_oneof_index = field_oneof_index
        elif f.HasField('oneof_index'):
            current_oneof_index = f.oneof_index
        target_proto.field.add().MergeFrom(f)
        field_index += 1
    if current_oneof_index is not None:
        # No need to adjust source code info here, since there are no comments for
        # trailing deprecated fields, so just set field index to None.
        append_extra_oneof_fields(current_oneof_index, None)
    # Non-oneof fields are easy to treat, we just append them to the existing
    # fields. They don't get any comments, but that's fine in the generated
    # shadows.
    for f in extra_simple_fields:
        target_proto.field.add().MergeFrom(f)
    for oneof_index in sorted(extra_oneof_fields.keys()):
        for f in extra_oneof_fields[oneof_index]:
            target_proto.field.add().MergeFrom(f)
    # Same is true for oneofs that are exclusively from the shadow.
    adjust_reserved_range(target_proto, active_proto.reserved_range, skip_reserved_numbers)
    # Visit nested message types
    del target_proto.nested_type[:]
    shadow_msgs = {msg.name: msg for msg in shadow_proto.nested_type}
    for index, msg in enumerate(active_proto.nested_type):
        merge_active_shadow_message(
            type_context.extend_nested_message(index, msg.name, msg.options.deprecated), msg,
            shadow_msgs.get(msg.name), target_proto.nested_type.add(), target_proto_dependencies)
    # Visit nested enum types
    del target_proto.enum_type[:]
    shadow_enums = {msg.name: msg for msg in shadow_proto.enum_type}
    for enum in active_proto.enum_type:
        merge_active_shadow_enum(
            enum, shadow_enums.get(enum.name), target_proto.enum_type.add(),
            target_proto_dependencies)
    # Ensure target has any deprecated sub-message types in case they are needed.
    active_msg_names = set([msg.name for msg in active_proto.nested_type])
    for msg in shadow_proto.nested_type:
        if msg.name not in active_msg_names:
            target_proto.nested_type.add().MergeFrom(msg)


# Merge active/shadow FileDescriptorProtos, returning the resulting FileDescriptorProto.
def merge_active_shadow_file(active_file_proto, shadow_file_proto):
    target_file_proto = copy.deepcopy(active_file_proto)
    source_code_info = api_type_context.SourceCodeInfo(
        target_file_proto.name, target_file_proto.source_code_info)
    package_type_context = api_type_context.TypeContext(source_code_info, target_file_proto.package)
    # Visit message types
    del target_file_proto.message_type[:]
    shadow_msgs = {msg.name: msg for msg in shadow_file_proto.message_type}
    for index, msg in enumerate(active_file_proto.message_type):
        merge_active_shadow_message(
            package_type_context.extend_message(index, msg.name, msg.options.deprecated), msg,
            shadow_msgs.get(msg.name), target_file_proto.message_type.add(),
            target_file_proto.dependency)
    # Visit enum types
    del target_file_proto.enum_type[:]
    shadow_enums = {msg.name: msg for msg in shadow_file_proto.enum_type}
    for enum in active_file_proto.enum_type:
        merge_active_shadow_enum(
            enum, shadow_enums.get(enum.name), target_file_proto.enum_type.add(),
            target_file_proto.dependency)
    # Ensure target has any deprecated message types in case they are needed.
    active_msg_names = set([msg.name for msg in active_file_proto.message_type])
    for msg in shadow_file_proto.message_type:
        if msg.name not in active_msg_names:
            target_file_proto.message_type.add().MergeFrom(msg)
    return target_file_proto


if __name__ == '__main__':
    active_src, shadow_src, dst = sys.argv[1:]

    utils.load_protos(PROTO_PACKAGES)

    active_proto = descriptor_pb2.FileDescriptorProto()
    text_format.Merge(pathlib.Path(active_src).read_text(), active_proto)
    shadow_proto = descriptor_pb2.FileDescriptorProto()
    text_format.Merge(pathlib.Path(shadow_src).read_text(), shadow_proto)
    pathlib.Path(dst).write_text(str(merge_active_shadow_file(active_proto, shadow_proto)))
