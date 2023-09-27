// Copyright (c) 2016-2019, The Monero Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "daemon_messages.h"
#include "serialization/json_object.h"

namespace cryptonote
{

namespace rpc
{

const char* const GetHeight::name = "get_height";
const char* const GetBlocksFast::name = "get_blocks_fast";
const char* const GetHashesFast::name = "get_hashes_fast";
const char* const GetTransactions::name = "get_transactions";
const char* const KeyImagesSpent::name = "key_images_spent";
const char* const GetTxGlobalOutputIndices::name = "get_tx_global_output_indices";
const char* const SendRawTx::name = "send_raw_tx";
const char* const SendRawTxHex::name = "send_raw_tx_hex";
const char* const StartMining::name = "start_mining";
const char* const StopMining::name = "stop_mining";
const char* const MiningStatus::name = "mining_status";
const char* const GetInfo::name = "get_info";
const char* const SaveBC::name = "save_bc";
const char* const GetBlockHash::name = "get_block_hash";
const char* const GetLastBlockHeader::name = "get_last_block_header";
const char* const GetBlockHeaderByHash::name = "get_block_header_by_hash";
const char* const GetBlockHeaderByHeight::name = "get_block_header_by_height";
const char* const GetBlockHeadersByHeight::name = "get_block_headers_by_height";
const char* const GetPeerList::name = "get_peer_list";
const char* const SetLogLevel::name = "set_log_level";
const char* const GetTransactionPool::name = "get_transaction_pool";
const char* const HardForkInfo::name = "hard_fork_info";
const char* const GetOutputHistogram::name = "get_output_histogram";
const char* const GetOutputKeys::name = "get_output_keys";
const char* const GetRPCVersion::name = "get_rpc_version";
const char* const GetFeeEstimate::name = "get_dynamic_fee_estimate";
const char* const GetOutputDistribution::name = "get_output_distribution";




rapidjson::Value GetHeight::Request::toJson(rapidjson::Document& doc) const
{
  return Message::toJson(doc);
}

void GetHeight::Request::fromJson(rapidjson::Value& val)
{
}

rapidjson::Value GetHeight::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  auto& al = doc.GetAllocator();

  val.AddMember("height", height, al);

  return val;
}

void GetHeight::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "height", height);
}


rapidjson::Value GetBlocksFast::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  auto& al = doc.GetAllocator();

  json::insert_into_json_object(val, doc, "block_ids", block_ids);
  val.AddMember("start_height", start_height, al);
  val.AddMember("prune", prune, al);

  return val;
}

void GetBlocksFast::Request::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "block_ids", block_ids);
  json::load_from_json_object(val, "start_height", start_height);
  json::load_from_json_object(val, "prune", prune);
}

rapidjson::Value GetBlocksFast::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  auto& al = doc.GetAllocator();

  json::insert_into_json_object(val, doc, "blocks", blocks);
  val.AddMember("start_height", start_height, al);
  val.AddMember("current_height", current_height, al);
  json::insert_into_json_object(val, doc, "output_indices", output_indices);

  return val;
}

void GetBlocksFast::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "blocks", blocks);
  json::load_from_json_object(val, "start_height", start_height);
  json::load_from_json_object(val, "current_height", current_height);
  json::load_from_json_object(val, "output_indices", output_indices);
}


rapidjson::Value GetHashesFast::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  auto& al = doc.GetAllocator();

  json::insert_into_json_object(val, doc, "known_hashes", known_hashes);
  val.AddMember("start_height", start_height, al);

  return val;
}

void GetHashesFast::Request::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "known_hashes", known_hashes);
  json::load_from_json_object(val, "start_height", start_height);
}

rapidjson::Value GetHashesFast::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  auto& al = doc.GetAllocator();

  json::insert_into_json_object(val, doc, "hashes", hashes);
  val.AddMember("start_height", start_height, al);
  val.AddMember("current_height", current_height, al);

  return val;
}

void GetHashesFast::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "hashes", hashes);
  json::load_from_json_object(val, "start_height", start_height);
  json::load_from_json_object(val, "current_height", current_height);
}


rapidjson::Value GetTransactions::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "tx_hashes", tx_hashes);

  return val;
}

void GetTransactions::Request::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "tx_hashes", tx_hashes);
}

rapidjson::Value GetTransactions::Response::toJson(rapidjson::Document& doc) const
{
  rapidjson::Value val(rapidjson::kObjectType);

  json::insert_into_json_object(val, doc, "txs", txs);
  json::insert_into_json_object(val, doc, "missed_hashes", missed_hashes);

  return val;
}

void GetTransactions::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "txs", txs);
  json::load_from_json_object(val, "missed_hashes", missed_hashes);
}


rapidjson::Value KeyImagesSpent::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "key_images", key_images);

  return val;
}

void KeyImagesSpent::Request::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "key_images", key_images);
}

rapidjson::Value KeyImagesSpent::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "spent_status", spent_status);

  return val;
}

void KeyImagesSpent::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "spent_status", spent_status);
}


rapidjson::Value GetTxGlobalOutputIndices::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "tx_hash", tx_hash);

  return val;
}

void GetTxGlobalOutputIndices::Request::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "tx_hash", tx_hash);
}

rapidjson::Value GetTxGlobalOutputIndices::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "output_indices", output_indices);

  return val;
}

void GetTxGlobalOutputIndices::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "output_indices", output_indices);
}

rapidjson::Value SendRawTx::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "tx", tx);
  json::insert_into_json_object(val, doc, "relay", relay);

  return val;
}

void SendRawTx::Request::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "tx", tx);
  json::load_from_json_object(val, "relay", relay);
}

rapidjson::Value SendRawTx::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "relayed", relayed);

  return val;
}


void SendRawTx::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "relayed", relayed);
}

rapidjson::Value SendRawTxHex::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "tx_as_hex", tx_as_hex);
  json::insert_into_json_object(val, doc, "relay", relay);

  return val;
}

void SendRawTxHex::Request::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "tx_as_hex", tx_as_hex);
  json::load_from_json_object(val, "relay", relay);
}

rapidjson::Value StartMining::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "miner_address", miner_address);
  json::insert_into_json_object(val, doc, "threads_count", threads_count);

  return val;
}

void StartMining::Request::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "miner_address", miner_address);
  json::load_from_json_object(val, "threads_count", threads_count);
}

rapidjson::Value StartMining::Response::toJson(rapidjson::Document& doc) const
{
  return Message::toJson(doc);
}

void StartMining::Response::fromJson(rapidjson::Value& val)
{
}


rapidjson::Value StopMining::Request::toJson(rapidjson::Document& doc) const
{
  return Message::toJson(doc);
}

void StopMining::Request::fromJson(rapidjson::Value& val)
{
}

rapidjson::Value StopMining::Response::toJson(rapidjson::Document& doc) const
{
  return Message::toJson(doc);
}

void StopMining::Response::fromJson(rapidjson::Value& val)
{
}


rapidjson::Value MiningStatus::Request::toJson(rapidjson::Document& doc) const
{
  return Message::toJson(doc);
}

void MiningStatus::Request::fromJson(rapidjson::Value& val)
{
}

rapidjson::Value MiningStatus::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "active", active);
  json::insert_into_json_object(val, doc, "speed", speed);
  json::insert_into_json_object(val, doc, "threads_count", threads_count);
  json::insert_into_json_object(val, doc, "address", address);

  return val;
}

void MiningStatus::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "active", active);
  json::load_from_json_object(val, "speed", speed);
  json::load_from_json_object(val, "threads_count", threads_count);
  json::load_from_json_object(val, "address", address);
}


rapidjson::Value GetInfo::Request::toJson(rapidjson::Document& doc) const
{
  return Message::toJson(doc);
}

void GetInfo::Request::fromJson(rapidjson::Value& val)
{
}

rapidjson::Value GetInfo::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "info", info);

  return val;
}

void GetInfo::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "info", info);
}


rapidjson::Value SaveBC::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  return val;
}

void SaveBC::Request::fromJson(rapidjson::Value& val)
{
}

rapidjson::Value SaveBC::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  return val;
}

void SaveBC::Response::fromJson(rapidjson::Value& val)
{
}


rapidjson::Value GetBlockHash::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "height", height);

  return val;
}

void GetBlockHash::Request::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "height", height);
}

rapidjson::Value GetBlockHash::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "hash", hash);

  return val;
}

void GetBlockHash::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "hash", hash);
}


rapidjson::Value GetLastBlockHeader::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  return val;
}

void GetLastBlockHeader::Request::fromJson(rapidjson::Value& val)
{
}

rapidjson::Value GetLastBlockHeader::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "header", header);

  return val;
}

void GetLastBlockHeader::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "header", header);
}


rapidjson::Value GetBlockHeaderByHash::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "hash", hash);

  return val;
}

void GetBlockHeaderByHash::Request::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "hash", hash);
}

rapidjson::Value GetBlockHeaderByHash::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "header", header);

  return val;
}

void GetBlockHeaderByHash::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "header", header);
}


rapidjson::Value GetBlockHeaderByHeight::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "height", height);

  return val;
}

void GetBlockHeaderByHeight::Request::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "height", height);
}

rapidjson::Value GetBlockHeaderByHeight::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "header", header);

  return val;
}

void GetBlockHeaderByHeight::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "header", header);
}


rapidjson::Value GetBlockHeadersByHeight::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "heights", heights);

  return val;
}

void GetBlockHeadersByHeight::Request::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "heights", heights);
}

rapidjson::Value GetBlockHeadersByHeight::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "headers", headers);

  return val;
}

void GetBlockHeadersByHeight::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "headers", headers);
}


rapidjson::Value GetPeerList::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  return val;
}

void GetPeerList::Request::fromJson(rapidjson::Value& val)
{
}

rapidjson::Value GetPeerList::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "white_list", white_list);
  json::insert_into_json_object(val, doc, "gray_list", gray_list);

  return val;
}

void GetPeerList::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "white_list", white_list);
  json::load_from_json_object(val, "gray_list", gray_list);
}


rapidjson::Value SetLogLevel::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  auto& al = doc.GetAllocator();

  val.AddMember("level", level, al);

  return val;
}

void SetLogLevel::Request::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "level", level);
}

rapidjson::Value SetLogLevel::Response::toJson(rapidjson::Document& doc) const
{
  return Message::toJson(doc);
}

void SetLogLevel::Response::fromJson(rapidjson::Value& val)
{
}


rapidjson::Value GetTransactionPool::Request::toJson(rapidjson::Document& doc) const
{
  return Message::toJson(doc);
}

void GetTransactionPool::Request::fromJson(rapidjson::Value& val)
{
}

rapidjson::Value GetTransactionPool::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "transactions", transactions);
  json::insert_into_json_object(val, doc, "key_images", key_images);

  return val;
}

void GetTransactionPool::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "transactions", transactions);
  json::load_from_json_object(val, "key_images", key_images);
}


rapidjson::Value HardForkInfo::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "version", version);

  return val;
}

void HardForkInfo::Request::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "version", version);
}

rapidjson::Value HardForkInfo::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "info", info);

  return val;
}

void HardForkInfo::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "info", info);
}


rapidjson::Value GetOutputHistogram::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "amounts", amounts);
  json::insert_into_json_object(val, doc, "min_count", min_count);
  json::insert_into_json_object(val, doc, "max_count", max_count);
  json::insert_into_json_object(val, doc, "unlocked", unlocked);
  json::insert_into_json_object(val, doc, "recent_cutoff", recent_cutoff);

  return val;
}

void GetOutputHistogram::Request::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "amounts", amounts);
  json::load_from_json_object(val, "min_count", min_count);
  json::load_from_json_object(val, "max_count", max_count);
  json::load_from_json_object(val, "unlocked", unlocked);
  json::load_from_json_object(val, "recent_cutoff", recent_cutoff);
}

rapidjson::Value GetOutputHistogram::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "histogram", histogram);

  return val;
}

void GetOutputHistogram::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "histogram", histogram);
}


rapidjson::Value GetOutputKeys::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "outputs", outputs);

  return val;
}

void GetOutputKeys::Request::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "outputs", outputs);
}

rapidjson::Value GetOutputKeys::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "keys", keys);

  return val;
}

void GetOutputKeys::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "keys", keys);
}


rapidjson::Value GetRPCVersion::Request::toJson(rapidjson::Document& doc) const
{
  return Message::toJson(doc);
}

void GetRPCVersion::Request::fromJson(rapidjson::Value& val)
{
}

rapidjson::Value GetRPCVersion::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "version", version);

  return val;
}

void GetRPCVersion::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "version", version);
}

rapidjson::Value GetFeeEstimate::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "num_grace_blocks", num_grace_blocks);

  return val;
}

void GetFeeEstimate::Request::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "num_grace_blocks", num_grace_blocks);
}

rapidjson::Value GetFeeEstimate::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "estimated_base_fee_per_byte", estimated_base_fee_per_byte);
  json::insert_into_json_object(val, doc, "estimated_base_fee_per_output", estimated_base_fee_per_output);
  json::insert_into_json_object(val, doc, "fee_mask", fee_mask);
  json::insert_into_json_object(val, doc, "size_scale", size_scale);
  json::insert_into_json_object(val, doc, "hard_fork_version", hard_fork_version);

  return val;
}

void GetFeeEstimate::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "estimated_base_fee_per_byte", estimated_base_fee_per_byte);
  json::load_from_json_object(val, "estimated_base_fee_per_output", estimated_base_fee_per_output);
  json::load_from_json_object(val, "fee_mask", fee_mask);
  json::load_from_json_object(val, "size_scale", size_scale);
  json::load_from_json_object(val, "hard_fork_version", hard_fork_version);
}

rapidjson::Value GetOutputDistribution::Request::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "amounts", amounts);
  json::insert_into_json_object(val, doc, "from_height", from_height);
  json::insert_into_json_object(val, doc, "to_height", to_height);
  json::insert_into_json_object(val, doc, "cumulative", cumulative);

  return val;
}

void GetOutputDistribution::Request::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "amounts", amounts);
  json::load_from_json_object(val, "from_height", from_height);
  json::load_from_json_object(val, "to_height", to_height);
  json::load_from_json_object(val, "cumulative", cumulative);
}

rapidjson::Value GetOutputDistribution::Response::toJson(rapidjson::Document& doc) const
{
  auto val = Message::toJson(doc);

  json::insert_into_json_object(val, doc, "status", status);
  json::insert_into_json_object(val, doc, "distributions", distributions);

  return val;
}

void GetOutputDistribution::Response::fromJson(rapidjson::Value& val)
{
  json::load_from_json_object(val, "status", status);
  json::load_from_json_object(val, "distributions", distributions);
}

}  // namespace rpc

}  // namespace cryptonote
