#include "extensions/filters/network/mysql_proxy/mysql_codec_switch_resp.h"

#include "envoy/buffer/buffer.h"

#include "common/common/logger.h"

#include "extensions/filters/network/mysql_proxy/mysql_codec.h"
#include "extensions/filters/network/mysql_proxy/mysql_utils.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace MySQLProxy {

DecodeStatus ClientSwitchResponse::parseMessage(Buffer::Instance& buffer, uint32_t remain_len) {
  if (BufferHelper::readStringBySize(buffer, remain_len, auth_plugin_resp_) !=
      DecodeStatus::Success) {
    ENVOY_LOG(debug, "error when parsing auth plugin data of client switch response");
    return DecodeStatus::Failure;
  }
  return DecodeStatus::Success;
}

void ClientSwitchResponse::encode(Buffer::Instance& out) const {
  BufferHelper::addString(out, auth_plugin_resp_);
}

} // namespace MySQLProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
