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
#include "rvsactionbase.h"

#include <chrono>
#include <unistd.h>

#include "rvs_util.h"

using namespace std;

/**
 * @brief Default constructor.
 *
 * */
rvs::actionbase::actionbase() {
}

/**
 * @brief Default destructor.
 *
 * */
rvs::actionbase::~actionbase() {
}

/**
 * @brief Sets action property
 *
 * @param pKey Property key
 * @param pVal Property value
 * @return 0 - success. non-zero otherwise
 *
 * */
int rvs::actionbase::property_set(const char* pKey, const char* pVal) {
  property.insert( pair<string, string>(pKey, pVal));
  return 0;
}

/**
 * @brief Pauses current thread for the given time period
 *
 * @param ms Sleep time in milliseconds.
 * @return (void)
 *
 * */
void rvs::actionbase::sleep(const unsigned int ms) {
  ::usleep(1000*ms);
}

/**
 * @brief Checks if property is set.
 *
 * Returns value if propety is set.
 *
 * @param key Property key
 * @param val Property value. Not changed if propery is not set.
 * @return TRUE - property set, FALSE - othewise
 *
 * */
bool rvs::actionbase::has_property(const std::string& key, std::string& val) {

  auto it = property.find(key);
  if(it != property.end()) {
    val = it->second;
    return true;
  }

  return false;
}

/**
 * @brief Checks if property is set.
 *
 * @param key Property key
 * @return TRUE - property set, FALSE - othewise
 *
 * */
bool rvs::actionbase::has_property(const std::string& key) {
  string val;
  return has_property(key, val);
}

/**
 * gets the deviceid from the module's properties collection
 * @param error pointer to a memory location where the error code will be stored
 * @return deviceid value if valid, -1 otherwise
 */
int rvs::actionbase::property_get_deviceid(int *error) {
    map<string, string>::iterator it = property.find(RVS_CONF_DEVICEID_KEY);
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
        property.erase(it);
    }

    return deviceid;
}

/**
 * gets the gpu_id list from the module's properties collection
 * @param error pointer to a memory location where the error code will be stored
 * @return true if "all" is selected, false otherwise
 */
bool rvs::actionbase::property_get_device(int *error) {
    map<string, string>::iterator it;  // module's properties map iterator
    *error = 0;  // init with 'no error'
    it = property.find(RVS_CONF_DEVICE_KEY);
    if (it != property.end()) {
        if (it->second == "all") {
            property.erase(it);
            return true;
        } else {
            // split the list of gpu_id
            device_prop_gpu_id_list = str_split(it->second,
                    YAML_DEVICE_PROP_DELIMITER);
            property.erase(it);

            if (device_prop_gpu_id_list.empty()) {
                *error = 1;  // list of gpu_id cannot be empty
            } else {
                for (vector<string>::iterator it_gpu_id =
                        device_prop_gpu_id_list.begin();
                        it_gpu_id != device_prop_gpu_id_list.end(); ++it_gpu_id)
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


