#!/usr/bin/env python3

# Generate an extension database, a JSON file mapping from qualified well known
# extension name to metadata derived from the envoy_cc_extension target.

# This script expects a copy of the envoy source to be located at /source
# Alternatively, you can specify a path to the source dir with `ENVOY_SRCDIR`

# You must specify the target file to save the generated json db to.
# You can do this either as an arg to this script/target or with the env var
# `EXTENSION_DB_PATH`

import ast
import json
import os
import pathlib
import re
import subprocess
import sys

from importlib.util import spec_from_loader, module_from_spec
from importlib.machinery import SourceFileLoader

BUILDOZER_PATH = os.path.abspath(
    "external/com_github_bazelbuild_buildtools/buildozer/buildozer_/buildozer")

ENVOY_SRCDIR = os.getenv('ENVOY_SRCDIR', '/source')

if not os.path.exists(ENVOY_SRCDIR):
    raise SystemExit(
        "Envoy source must either be located at /source, or ENVOY_SRCDIR env var must be set")

# source/extensions/extensions_build_config.bzl must have a .bzl suffix for Starlark
# import, so we are forced to do this workaround.
_extensions_build_config_spec = spec_from_loader(
    'extensions_build_config',
    SourceFileLoader(
        'extensions_build_config',
        os.path.join(ENVOY_SRCDIR, 'source/extensions/extensions_build_config.bzl')))
extensions_build_config = module_from_spec(_extensions_build_config_spec)
_extensions_build_config_spec.loader.exec_module(extensions_build_config)


class ExtensionDbError(Exception):
    pass


def is_missing(value):
    return value == '(missing)'


def num_read_filters_fuzzed():
    data = pathlib.Path(
        os.path.join(
            ENVOY_SRCDIR,
            'test/extensions/filters/network/common/fuzz/uber_per_readfilter.cc')).read_text()
    # Hack-ish! We only search the first 50 lines to capture the filters in filterNames().
    return len(re.findall('NetworkFilterNames::get()', ''.join(data.splitlines()[:50])))


def num_robust_to_downstream_network_filters(db):
    # Count number of network filters robust to untrusted downstreams.
    return len([
        ext for ext, data in db.items()
        if 'network' in ext and data['security_posture'] == 'robust_to_untrusted_downstream'
    ])


def get_extension_metadata(target):
    if not BUILDOZER_PATH:
        raise ExtensionDbError('Buildozer not found!')
    r = subprocess.run(
        [BUILDOZER_PATH, '-stdout', 'print security_posture status undocumented category', target],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE)
    rout = r.stdout.decode('utf-8').strip().split(' ')
    security_posture, status, undocumented = rout[:3]
    categories = ' '.join(rout[3:])
    if is_missing(security_posture):
        raise ExtensionDbError(
            'Missing security posture for %s.  Please make sure the target is an envoy_cc_extension and security_posture is set'
            % target)
    if is_missing(categories):
        raise ExtensionDbError(
            'Missing extension category for %s. Please make sure the target is an envoy_cc_extension and category is set'
            % target)
    # evaluate tuples/lists
    # wrap strings in a list
    categories = (
        ast.literal_eval(categories) if ('[' in categories or '(' in categories) else [categories])
    return {
        'security_posture': security_posture,
        'undocumented': False if is_missing(undocumented) else bool(undocumented),
        'status': 'stable' if is_missing(status) else status,
        'categories': categories,
    }


if __name__ == '__main__':
    try:
        output_path = os.getenv("EXTENSION_DB_PATH") or sys.argv[1]
    except IndexError:
        raise SystemExit(
            "Output path must be either specified as arg or with EXTENSION_DB_PATH env var")

    extension_db = {}
    # Include all extensions from source/extensions/extensions_build_config.bzl
    all_extensions = {}
    all_extensions.update(extensions_build_config.EXTENSIONS)
    for extension, target in all_extensions.items():
        extension_db[extension] = get_extension_metadata(target)
    if num_robust_to_downstream_network_filters(extension_db) != num_read_filters_fuzzed():
        raise ExtensionDbError(
            'Check that all network filters robust against untrusted'
            'downstreams are fuzzed by adding them to filterNames() in'
            'test/extensions/filters/network/common/uber_per_readfilter.cc')
    # The TLS and generic upstream extensions are hard-coded into the build, so
    # not in source/extensions/extensions_build_config.bzl
    # TODO(mattklein123): Read these special keys from all_extensions.bzl or a shared location to
    # avoid duplicate logic.
    extension_db['envoy.transport_sockets.tls'] = get_extension_metadata(
        '//source/extensions/transport_sockets/tls:config')
    extension_db['envoy.upstreams.http.generic'] = get_extension_metadata(
        '//source/extensions/upstreams/http/generic:config')
    extension_db['envoy.upstreams.tcp.generic'] = get_extension_metadata(
        '//source/extensions/upstreams/tcp/generic:config')
    extension_db['envoy.upstreams.http.http_protocol_options'] = get_extension_metadata(
        '//source/extensions/upstreams/http:config')
    extension_db['envoy.request_id.uuid'] = get_extension_metadata(
        '//source/extensions/request_id/uuid:config')

    pathlib.Path(os.path.dirname(output_path)).mkdir(parents=True, exist_ok=True)
    pathlib.Path(output_path).write_text(json.dumps(extension_db))
