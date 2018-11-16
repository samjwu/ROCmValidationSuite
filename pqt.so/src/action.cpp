/********************************************************************************
 *
 * Copyright (c) 2018 ROCm Developer Tools
 *
 * MIT LICENSE:
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/
#include "action.h"

extern "C" {
#include <pci/pci.h>
#include <linux/pci.h>
}
#include <stdio.h>
#include <stdlib.h>

#include <iostream>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "rvs_key_def.h"
#include "pci_caps.h"
#include "gpu_util.h"
#include "rvs_util.h"
#include "rvsloglp.h"
#include "rvshsa.h"
#include "rvstimer.h"

#include "rvs_module.h"
#include "worker.h"
#include "worker_b2b.h"


#define MODULE_NAME "pqt"
#define MODULE_NAME_CAPS "PQT"
#define JSON_CREATE_NODE_ERROR "JSON cannot create node"

using std::string;
using std::vector;

//! Default constructor
pqtaction::pqtaction() {
  prop_deviceid = -1;
  prop_device_id_filtering = false;
  prop_peer_deviceid = -1;
  bjson = false;
}

//! Default destructor
pqtaction::~pqtaction() {
  property.clear();
}

/**
 * gets the peer gpu_id list from the module's properties collection
 * @param error pointer to a memory location where the error code will be stored
 * @return true if "all" is selected, false otherwise
 */
bool pqtaction::property_get_peers(int *error) {
    *error = 0;  // init with 'no error'
    auto it = property.find("peers");
    if (it != property.end()) {
        if (it->second == "all") {
            return true;
        } else {
            // split the list of gpu_id
            prop_peers = str_split(it->second,
                    YAML_DEVICE_PROP_DELIMITER);
            if (prop_peers.empty()) {
                *error = 1;  // list of gpu_id cannot be empty
            } else {
                for (vector<string>::iterator it_gpu_id =
                        prop_peers.begin();
                        it_gpu_id != prop_peers.end(); ++it_gpu_id)
                    if (!is_positive_integer(*it_gpu_id)) {
                        *error = 1;
                        break;
                    }
            }
            return false;
        }

    } else {
        *error = 1;
        // when error is set, it doesn't really matter whether the method
        // returns true or false
        return false;
    }
}

/**
 * gets the peer deviceid from the module's properties collection
 * @param error pointer to a memory location where the error code will be stored
 * @return deviceid value if valid, -1 otherwise
 */
/*int pqtaction::property_get_peer_deviceid(int *error) {
    auto it = property.find("peer_deviceid");
    int deviceid = -1;
    *error = 0;  // init with 'no error'

    if (it != property.end()) {
        if (it->second != "") {
            if (is_positive_integer(it->second)) {
                deviceid = std::stoi(it->second);
            } else {
                *error = 1;  // we have something but it's not a number
            }
        } else {
            *error = 1;  // we have an empty string
        }
    }
    return deviceid;
}*/

/**
 * @brief reads the module's properties collection to see whether bandwidth
 * tests should be run after peer check
 */
void pqtaction::property_get_test_bandwidth(int *error) {
  prop_test_bandwidth = false;
  auto it = property.find("test_bandwidth");
  if (it != property.end()) {
    if (it->second == "true") {
      prop_test_bandwidth = true;
      *error = 0;
    } else if (it->second == "false") {
      *error = 0;
    } else {
      *error = 1;
    }
  } else {
    *error = 2;
  }
}

/**
 * @brief reads the module's properties collection to see whether bandwidth
 * tests should be run in both directions
 */
void pqtaction::property_get_bidirectional(int *error) {
  prop_bidirectional = false;
  auto it = property.find("bidirectional");
  if (it != property.end()) {
    if (it->second == "true") {
      prop_bidirectional = true;
      *error = 0;
    } else if (it->second == "false") {
      *error = 0;
    } else {
      *error = 1;
    }
  } else {
    *error = 2;
  }
}

/**
 * @brief reads all PQT related configuration keys from
 * the module's properties collection
 * @return true if no fatal error occured, false otherwise
 */
bool pqtaction::get_all_pqt_config_keys(void) {
  int    error;
  string msg;

  prop_peer_device_all_selected = property_get_peers(&error);
  if (error) {
    msg =  "invalid peers";
    rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
    return false;
  }

  std::string val;
  if (has_property("peer_deviceid", &val)) {
  error = property_get_int<int>("peer_deviceid", &prop_peer_deviceid);
  }
  if (error != 0) {
    msg = "invalid 'peer_deviceid '";
    rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
    return false;
  }

  property_get_test_bandwidth(&error);
  if (error) {
    msg = "invalid 'test_bandwidth'";
    rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
    return false;
  }

  error = property_get_int_default<int>
  (RVS_CONF_LOG_INTERVAL_KEY, &prop_log_interval, DEFAULT_LOG_INTERVAL);
  if (error == 1) {
    msg = "invalid '" + std::string(RVS_CONF_LOG_INTERVAL_KEY) + "'";
    rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
    return false;
  }

  property_get_bidirectional(&error);
  if (error) {
    if (prop_test_bandwidth == true) {
      msg = "invalid 'bidirectional'";
      rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
      return false;
    }
  }

  property_get_uint_list(RVS_CONF_BLOCK_SIZE_KEY, YAML_DEVICE_PROP_DELIMITER,
                         &block_size, &b_block_size_all, &error);
  if (error == 1) {
      msg =  "invalid '" + std::string(RVS_CONF_BLOCK_SIZE_KEY) + "' key";
      rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
      return false;
  } else if (error == 2) {
    b_block_size_all = true;
    block_size.clear();
  }

  error = property_get_int<uint32_t>
  (RVS_CONF_B2B_BLOCK_SIZE_KEY, &b2b_block_size);
  if (error == 1) {
    msg =  "invalid '" + std::string(RVS_CONF_B2B_BLOCK_SIZE_KEY) + "' key";
    rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
    return false;
  }

  error = property_get_int<int>(RVS_CONF_LINK_TYPE_KEY, &link_type);
  if (error == 1) {
    msg =  "invalid '" + std::string(RVS_CONF_LINK_TYPE_KEY) + "' key";
    rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
    return false;
  }

  return true;
}

/**
 * @brief reads all common configuration keys from
 * the module's properties collection
 * @return true if no fatal error occured, false otherwise
 */
bool pqtaction::get_all_common_config_keys(void) {
  string msg, sdevid, sdev;
  int error;

  // get the action name
  property_get_action_name(&error);
  if (error) {
    msg = "action field is missing";
    rvs::lp::Err(msg, MODULE_NAME_CAPS);
    return false;
  }

  // get <device> property value (a list of gpu id)
  if (has_property("device", &sdev)) {
      prop_device_all_selected = property_get_device(&error);
      if (error) {  // log the error & abort GST
          msg = "invalid 'device' key value " + sdev;
          rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
          return false;
      }
  } else {
      msg = "key 'device' was not found";
      rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
      return false;
  }

  int devid;
  // get the <deviceid> property value
  if (has_property("deviceid", &sdevid)) {
    error = property_get_int<int>(RVS_CONF_DEVICEID_KEY, &devid);
      if (error == 0) {
          if (devid != -1) {
              prop_deviceid = static_cast<uint16_t>(devid);
              prop_device_id_filtering = true;
          }
      } else {
          msg = "invalid 'deviceid' key value " + sdevid;
          rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
          return false;
      }
  }

  // get the other action/GST related properties
  rvs::actionbase::property_get_run_parallel(&error);
  if (error == 1) {
      msg = "invalid '" + std::string(RVS_CONF_PARALLEL_KEY) +
          "' key value";
      rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
      return false;
  }

  error = property_get_int_default<uint64_t>
  (RVS_CONF_COUNT_KEY, &gst_run_count, 1);
  if (error == 1) {
      msg = "invalid '" + std::string(RVS_CONF_COUNT_KEY) + "' key value";
      rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
      return false;
  }

  error = property_get_int_default<uint64_t>
  (RVS_CONF_WAIT_KEY, &gst_run_wait_ms, 0);
  if (error == 1) {
      msg = "invalid '" + std::string(RVS_CONF_WAIT_KEY) + "' key value";
      rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
      return false;
  }

  error = property_get_int_default<uint64_t>
  (RVS_CONF_DURATION_KEY, &gst_run_duration_ms, DEFAULT_DURATION);
  if (error == 1) {
      msg = "invalid '" + std::string(RVS_CONF_DURATION_KEY) +
          "' key value";
      rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
      return false;
  }

  return true;
}

/**
 * @brief Create thread objects based on action description in configuation
 * file.
 *
 * Threads are created but are not started. Execution, one by one of parallel,
 * depends on "parallel" key in configuration file. Pointers to created objects
 * are stored in "test_array" member
 *
 * @return 0 - if successfull, non-zero otherwise
 *
 * */
int pqtaction::create_threads() {
  std::string msg;

  std::vector<uint16_t> gpu_id;
  std::vector<uint16_t> gpu_device_id;
  uint16_t transfer_ix = 0;
  bool bmatch_found = false;

  gpu_get_all_gpu_id(&gpu_id);
  gpu_get_all_device_id(&gpu_device_id);

  for (size_t i = 0; i < gpu_id.size(); i++) {    // all possible sources
    // filter out by source device id
    if (prop_device_id_filtering) {
      if (prop_deviceid != gpu_device_id[i]) {
        continue;
      }
    }

    // filter out by listed sources
    if (!prop_device_all_selected) {
      const auto it = std::find(device_prop_gpu_id_list.cbegin(),
                                device_prop_gpu_id_list.cend(),
                                std::to_string(gpu_id[i]));
      if (it == device_prop_gpu_id_list.cend()) {
            continue;
      }
    }

    for (size_t j = 0; j < gpu_id.size(); j++) {  // all possible peers
      RVSTRACE_
      // filter out by peer id
      if (prop_peer_deviceid > 0) {
        RVSTRACE_
        if (prop_peer_deviceid != gpu_device_id[j]) {
          RVSTRACE_
          continue;
        }
      }

      RVSTRACE_
      // filter out by listed peers
      if (!prop_peer_device_all_selected) {
        RVSTRACE_
        const auto it = std::find(prop_peers.cbegin(),
                                  prop_peers.cend(),
                                  std::to_string(gpu_id[j]));
        if (it == prop_peers.cend()) {
          RVSTRACE_
          continue;
        }
      }

      RVSTRACE_
      // signal that at lease one matching src-dst combination
      // has been found:
      bmatch_found = true;

      // get NUMA nodes
      int srcnode = rvs::gpulist::GetNodeIdFromGpuId(gpu_id[i]);
      if (srcnode < 0) {
        msg + "no node found for GPU ID " + std::to_string(gpu_id[i]);
        rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
        return -1;
      }

      int dstnode = rvs::gpulist::GetNodeIdFromGpuId(gpu_id[j]);
      if (srcnode < 0) {
        RVSTRACE_
        msg = "no node found for GPU ID " + std::to_string(gpu_id[j]);
        rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
        return -1;
      }

      RVSTRACE_
      uint32_t distance = 0;
      std::vector<rvs::linkinfo_t> arr_linkinfo;
      rvs::hsa::Get()->GetLinkInfo(srcnode, dstnode,
                                         &distance, &arr_linkinfo);

      // perform peer check
      if (is_peer(gpu_id[i], gpu_id[j])) {
        RVSTRACE_
        msg = "[" + action_name + "] p2p "
            + std::to_string(gpu_id[i]) + " "
            + std::to_string(gpu_id[j]) + " peers:true ";

        if (distance == rvs::hsa::NO_CONN) {
          msg += "distance:-1";
        } else {
          msg += "distance:" + std::to_string(distance);
        }
        // iterate through individual hops
        for (auto it = arr_linkinfo.begin(); it != arr_linkinfo.end(); it++) {
          msg += " " + it->strtype + ":";
          if (it->distance == rvs::hsa::NO_CONN) {
            msg += "-1";
          } else {
            msg +=std::to_string(it->distance);
          }
        }
        rvs::lp::Log(msg, rvs::logresults);

        if (bjson) {
          RVSTRACE_
          unsigned int sec;
          unsigned int usec;
          rvs::lp::get_ticks(&sec, &usec);
          void* pjson = rvs::lp::LogRecordCreate(MODULE_NAME,
                              action_name.c_str(), rvs::logresults, sec, usec);
          if (pjson != NULL) {
            RVSTRACE_
            rvs::lp::AddString(pjson, "src",
                               std::to_string(gpu_id[i]));
            rvs::lp::AddString(pjson, "dst",
                               std::to_string(gpu_id[j]));
            rvs::lp::AddString(pjson, "p2p", "true");
            if (distance == rvs::hsa::NO_CONN) {
                rvs::lp::AddInt(pjson, "distance", -1);
            } else {
                rvs::lp::AddInt(pjson, "distance", distance);
            }

            void* phops = rvs::lp::CreateNode(pjson, "hops");
            rvs::lp::AddNode(pjson, phops);

            // iterate through individual hops
            for (uint i = 0; i < arr_linkinfo.size(); i++) {
              char sbuff[64];
              snprintf(sbuff, sizeof(sbuff), "hop%d", i);
             void* phop = rvs::lp::CreateNode(phops, sbuff);
              rvs::lp::AddString(phop, "type", arr_linkinfo[i].strtype);
              if (arr_linkinfo[i].distance == rvs::hsa::NO_CONN) {
                rvs::lp::AddInt(phop, "distance", -1);
              } else {
                rvs::lp::AddInt(phop, "distance", arr_linkinfo[i].distance);
              }
             rvs::lp::AddNode(phops, phop);
            }

            rvs::lp::LogRecordFlush(pjson);
          }
        }

        RVSTRACE_
        // GPUs are peers, create transaction for them
        if (prop_test_bandwidth) {
          RVSTRACE_
          pqtworker* p = nullptr;

          transfer_ix += 1;
          if (b2b_block_size > 0 && gst_runs_parallel) {
            RVSTRACE_
            pqtworker_b2b* pb2b = new pqtworker_b2b;
            if (pb2b == nullptr) {
              RVSTRACE_
              msg = "internal error";
              rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
              return -1;
            }
            pb2b->initialize(srcnode, dstnode, prop_bidirectional,
                             b2b_block_size);
            p = pb2b;

          } else {
            RVSTRACE_
            p = new pqtworker;
            if (p == nullptr) {
              RVSTRACE_
              msg = "internal error";
              rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
              return -1;
            }
            p->initialize(srcnode, dstnode, prop_bidirectional);
          }
          RVSTRACE_
          p->set_name(action_name);
          p->set_stop_name(action_name);
          p->set_transfer_ix(transfer_ix);
          p->set_block_sizes(block_size);
          test_array.push_back(p);
        }

      } else {
        RVSTRACE_
        msg = "[" + action_name + "] p2p "
            + std::to_string(gpu_id[i]) + " "
            + std::to_string(gpu_id[j]) + " peers:false ";

        if (distance == rvs::hsa::NO_CONN) {
          msg += "distance:-1";
        } else {
          msg += "distance:" + std::to_string(distance);
        }
        // iterate through individual hops
        for (auto it = arr_linkinfo.begin(); it != arr_linkinfo.end(); it++) {
          msg += " " + it->strtype + ":";
          if (it->distance == rvs::hsa::NO_CONN) {
            msg += "-1";
          } else {
            msg +=std::to_string(it->distance);
          }
        }

        rvs::lp::Log(msg, rvs::logresults);

        if (bjson) {
          RVSTRACE_
          unsigned int sec;
          unsigned int usec;
          rvs::lp::get_ticks(&sec, &usec);
          void* pjson = rvs::lp::LogRecordCreate(MODULE_NAME,
                              action_name.c_str(), rvs::logresults, sec, usec);
          if (pjson != NULL) {
            RVSTRACE_
            rvs::lp::AddString(pjson,
                               "src", std::to_string(gpu_id[i]));
            rvs::lp::AddString(pjson,
                               "dst", std::to_string(gpu_id[j]));
            rvs::lp::AddString(pjson,
                               "p2p", "false");
            if (distance == rvs::hsa::NO_CONN) {
                rvs::lp::AddInt(pjson, "distance", -1);
            } else {
                rvs::lp::AddInt(pjson, "distance", distance);
            }

            void* phops = rvs::lp::CreateNode(pjson, "hops");
            rvs::lp::AddNode(pjson, phops);

            // iterate through individual hops
            for (uint i = 0; i < arr_linkinfo.size(); i++) {
              char sbuff[64];
              snprintf(sbuff, sizeof(sbuff), "hop%d", i);
             void* phop = rvs::lp::CreateNode(phops, sbuff);
              rvs::lp::AddString(phop, "type", arr_linkinfo[i].strtype);
              if (arr_linkinfo[i].distance == rvs::hsa::NO_CONN) {
                rvs::lp::AddInt(phop, "distance", -1);
              } else {
                rvs::lp::AddInt(phop, "distance", arr_linkinfo[i].distance);
              }
             rvs::lp::AddNode(phops, phop);
            }

            rvs::lp::LogRecordFlush(pjson);
          }
        }
      }
    }
  }

  RVSTRACE_
  if (prop_test_bandwidth && test_array.size() < 1) {
    RVSTRACE_
    std::string diag;
    if (bmatch_found) {
      RVSTRACE_
      diag = "No peers found";
    } else {
      RVSTRACE_
      diag = "No devices match criteria from the test configuation";
    }
    RVSTRACE_
    msg = "[" + action_name + "] p2p-bandwidth " + diag;
    rvs::lp::Log(msg, rvs::logerror);
    if (bjson) {
      RVSTRACE_
      unsigned int sec;
      unsigned int usec;
      rvs::lp::get_ticks(&sec, &usec);
      void* pjson = rvs::lp::LogRecordCreate("p2p-bandwidth",
                              action_name.c_str(), rvs::logerror, sec, usec);
      if (pjson != NULL) {
        RVSTRACE_
        rvs::lp::AddString(pjson,
          "message",
          diag);
        rvs::lp::LogRecordFlush(pjson);
      }
    }
    RVSTRACE_
    return 0;
  }

  RVSTRACE_
  for (auto it = test_array.begin(); it != test_array.end(); ++it) {
    RVSTRACE_
    (*it)->set_transfer_num(test_array.size());
  }

  RVSTRACE_
  return 0;
}

/**
 * @brief Delete test thread objects at the end of action execution
 *
 * @return 0 - if successfull, non-zero otherwise
 *
 * */
int pqtaction::destroy_threads() {
  for (auto it = test_array.begin(); it != test_array.end(); ++it) {
    (*it)->set_stop_name(action_name);
    (*it)->stop();
    delete *it;
  }

  return 0;
}


/**
 * @brief Check if two GPU can access each other memory
 *
 * @param Src GPU ID of the source GPU
 * @param Dst GPU ID of the destination GPU
 *
 * @return 0 - no access, 1 - Src can acces Dst, 2 - both have access
 *
 * */
int pqtaction::is_peer(uint16_t Src, uint16_t Dst) {
  //! ptr to RVS HSA singleton wrapper
  rvs::hsa* pHsa;
  string msg;

  if (Src == Dst) {
    return 0;
  }
  pHsa = rvs::hsa::Get();

  // GPUs are peers, create transaction for them
  int srcnode = rvs::gpulist::GetNodeIdFromGpuId(Src);
  if (srcnode < 0) {
    msg = "no node found for GPU ID " + std::to_string(Src);
    rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
    return 0;
  }

  int dstnode = rvs::gpulist::GetNodeIdFromGpuId(Dst);
  if (srcnode < 0) {
    msg = "RVS-PQT: no node found for GPU ID " + std::to_string(Dst);
    rvs::lp::Err(msg, MODULE_NAME_CAPS, action_name);
    return 0;
  }

  return pHsa->rvs::hsa::GetPeerStatus(srcnode, dstnode);
}

/**
 * @brief Collect running average bandwidth data for all the tests and prints
 * them out every log_interval msecs.
 *
 * @return 0 - if successfull, non-zero otherwise
 *
 * */
int pqtaction::print_running_average() {
  for (auto it = test_array.begin(); brun && it != test_array.end(); ++it) {
    print_running_average(*it);
  }

  return 0;
}

/**
 * @brief Collect running average for this particular transfer.
 *
 * @param pWorker ptr to a pqtworker class
 *
 * @return 0 - if successfull, non-zero otherwise
 *
 * */
int pqtaction::print_running_average(pqtworker* pWorker) {
  int         src_node, dst_node;
  int         src_id, dst_id;
  bool        bidir;
  size_t      current_size;
  double      duration;
  std::string msg;
  char        buff[64];
  double      bandwidth;
  uint16_t    transfer_ix;
  uint16_t    transfer_num;

  // get running average
  pWorker->get_running_data(&src_node, &dst_node, &bidir,
                            &current_size, &duration);

  if (duration > 0) {
    bandwidth = current_size/duration/(1024*1024*1024);
    if (bidir) {
      bandwidth *=2;
    }
    snprintf( buff, sizeof(buff), "%.3f GBps", bandwidth);
  } else {
    // no running average in this iteration, try getting total so far
    // (do not reset final totals as this is just intermediate query)
    pWorker->get_final_data(&src_node, &dst_node, &bidir,
                            &current_size, &duration, false);
    if (duration > 0) {
      bandwidth = current_size/duration/(1024*1024*1024);
      if (bidir) {
        bandwidth *=2;
      }
      snprintf( buff, sizeof(buff), "%.3f GBps (*)", bandwidth);
    } else {
      // not transfers at all - print "pending"
      snprintf( buff, sizeof(buff), "(pending)");
    }
  }

  src_id = rvs::gpulist::GetGpuIdFromNodeId(src_node);
  dst_id = rvs::gpulist::GetGpuIdFromNodeId(dst_node);
  transfer_ix = pWorker->get_transfer_ix();
  transfer_num = pWorker->get_transfer_num();

  msg = "[" + action_name + "] p2p-bandwidth  ["
      + std::to_string(transfer_ix) + "/" + std::to_string(transfer_num)
      + "] " + std::to_string(src_id) + " " + std::to_string(dst_id)
      + "  bidirectional: " + std::string(bidir ? "true" : "false")
      + "  " + buff;
  rvs::lp::Log(msg, rvs::loginfo);
  if (bjson) {
    unsigned int sec;
    unsigned int usec;
    rvs::lp::get_ticks(&sec, &usec);
    void* pjson = rvs::lp::LogRecordCreate(MODULE_NAME,
                            action_name.c_str(), rvs::loginfo, sec, usec);
    if (pjson != NULL) {
      rvs::lp::AddString(pjson,
                          "transfer_ix", std::to_string(transfer_ix));
      rvs::lp::AddString(pjson,
                          "transfer_num", std::to_string(transfer_num));
      rvs::lp::AddString(pjson, "src", std::to_string(src_id));
      rvs::lp::AddString(pjson, "dst", std::to_string(dst_id));
      rvs::lp::AddString(pjson, "p2p", "true");
      rvs::lp::AddString(pjson, "bidirectional",
                          std::string(bidir ? "true" : "false"));
      rvs::lp::AddString(pjson, "bandwidth (GBs)", buff);
      rvs::lp::LogRecordFlush(pjson);
    }
  }

  return 0;
}

/**
 * @brief Collect bandwidth totals for all the tests and prints
 * them out at the end of action execution
 *
 * @return 0 - if successfull, non-zero otherwise
 *
 * */
int pqtaction::print_final_average() {
  int         src_node, dst_node;
  int         src_id, dst_id;
  bool        bidir;
  size_t      current_size;
  double      duration;
  std::string msg;
  double      bandwidth;
  char        buff[128];
  uint16_t    transfer_ix;
  uint16_t    transfer_num;

  for (auto it = test_array.begin(); it != test_array.end(); ++it) {
    (*it)->get_final_data(&src_node, &dst_node, &bidir,
                            &current_size, &duration);

    if (duration) {
      bandwidth = current_size/duration/(1024*1024*1024);
      if (bidir) {
        bandwidth *=2;
      }
      snprintf( buff, sizeof(buff), "%.3f GBps", bandwidth);
    } else {
      snprintf( buff, sizeof(buff), "(not measured)");
    }
    src_id = rvs::gpulist::GetGpuIdFromNodeId(src_node);
    dst_id = rvs::gpulist::GetGpuIdFromNodeId(dst_node);
    transfer_ix = (*it)->get_transfer_ix();
    transfer_num = (*it)->get_transfer_num();

    msg = "[" + action_name + "] p2p-bandwidth  ["
        + std::to_string(transfer_ix) + "/" + std::to_string(transfer_num)
        + "] " + std::to_string(src_id) + " " + std::to_string(dst_id)
        + "  bidirectional: " + std::string(bidir ? "true" : "false")
        + "  " + buff + "  duration: " + std::to_string(duration) + " sec";

    rvs::lp::Log(msg, rvs::logresults);
    if (bjson) {
      unsigned int sec;
      unsigned int usec;
      rvs::lp::get_ticks(&sec, &usec);
      void* pjson = rvs::lp::LogRecordCreate(MODULE_NAME,
                              action_name.c_str(), rvs::logresults, sec, usec);
      if (pjson != NULL) {
        rvs::lp::AddString(pjson,
                            "transfer_ix", std::to_string(transfer_ix));
        rvs::lp::AddString(pjson,
                            "transfer_num", std::to_string(transfer_num));
        rvs::lp::AddString(pjson, "src", std::to_string(src_id));
        rvs::lp::AddString(pjson, "dst", std::to_string(dst_id));
        rvs::lp::AddString(pjson, "p2p", "true");
        rvs::lp::AddString(pjson, "bidirectional",
                           std::string(bidir ? "true" : "false"));
        rvs::lp::AddString(pjson, "bandwidth (GBps)", buff);
        rvs::lp::AddString(pjson, "duration (sec)",
                           std::to_string(duration));
        rvs::lp::LogRecordFlush(pjson);
      }
    }
    sleep(1);
  }

  return 0;
}

/**
 * @brief timer callback used to signal end of test
 *
 * timer callback used to signal end of test and to initiate
 * calculation of final average
 *
 * */
void pqtaction::do_final_average() {
  std::string msg;
  unsigned int sec;
  unsigned int usec;
  rvs::lp::get_ticks(&sec, &usec);

  msg = "[" + action_name + "] pqt in do_final_average";
  rvs::lp::Log(msg, rvs::logtrace, sec, usec);

  if (bjson) {
    void* pjson = rvs::lp::LogRecordCreate(MODULE_NAME,
                            action_name.c_str(), rvs::logtrace, sec, usec);
    if (pjson != NULL) {
      rvs::lp::AddString(pjson, "message", "pqt in do_final_average");
      rvs::lp::LogRecordFlush(pjson);
    }
  }

  brun = false;

  // signal worker threads to stop
  for (auto it = test_array.begin(); it != test_array.end(); ++it) {
    (*it)->stop();
  }
}

/**
 * @brief timer callback used to signal end of log interval
 *
 * timer callback used to signal end of log interval and to initiate
 * calculation of moving average
 *
 * */
void pqtaction::do_running_average() {
  unsigned int sec;
  unsigned int usec;
  std::string msg;

  rvs::lp::get_ticks(&sec, &usec);
  msg = "[" + action_name + "] pqt in do_running_average";
  rvs::lp::Log(msg, rvs::logtrace, sec, usec);
  if (bjson) {
    void* pjson = rvs::lp::LogRecordCreate(MODULE_NAME,
                            action_name.c_str(), rvs::logtrace, sec, usec);
    if (pjson != NULL) {
      rvs::lp::AddString(pjson,
                         "message",
                         "in do_running_average");
      rvs::lp::LogRecordFlush(pjson);
    }
  }
  print_running_average();
}
