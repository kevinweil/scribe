//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//
// See accompanying file LICENSE or visit the Scribe site at:
// http://developers.facebook.com/scribe/
//
// @author Travis Crawford <travis@twitter.com>

#include "common.h"
#include "scribe_server.h"
#include "zk_client.h"
#include "zk_agg_selector.h"

using namespace std;
using boost::lexical_cast;
using boost::shared_ptr;

shared_ptr<ZKClient> g_ZKClient;

/*
 * Global Zookeeper watcher handles all callbacks.
 */
void watcher(zhandle_t *zzh, int type, int state,
    const char *path, void *watcherCtx) {

  // Zookeeper session established so attempt registration.
  if ((state == ZOO_CONNECTED_STATE) &&
      (type == ZOO_SESSION_EVENT)) {
    g_ZKClient->registerTask();
  }

  // Registration znode was deleted so attempt registration.
  else if ((state == ZOO_CONNECTED_STATE) &&
      (type == ZOO_DELETED_EVENT) &&
      (lexical_cast<string>(path) == g_ZKClient->zkFullRegistrationName)) {
    g_ZKClient->registerTask();
  }

  else if ((state == ZOO_EXPIRED_SESSION_STATE) && 
      (type == ZOO_SESSION_EVENT)) {
    g_ZKClient->disconnect();
		g_ZKClient->connect();
  }

  // This should never happen.
  else {
    LOG_OPER("Received unhandled watch callback: %s %d %d", path, type, state);
  }
}

ZKClient::ZKClient() {
  zh = NULL;
  if (debug_level) {
    zoo_set_debug_level(ZOO_LOG_LEVEL_DEBUG);
  }
}

ZKClient::~ZKClient() {
  zookeeper_close(zh);
}

void ZKClient::connect() {
  LOG_DEBUG("Connecting to zookeeper.");
  zh = zookeeper_init(zkServer.c_str(), watcher, 10000, 0, 0, 0);
}

void ZKClient::disconnect() {
  LOG_DEBUG("Disconnecting from zookeeper.");
  zookeeper_close(zh);
  zh = NULL;
}

bool ZKClient::registerTask() {
  if (zkRegistrationPrefix.empty()) {
    return false;
  }

  LOG_OPER("Registering task in Zookeeper.");
  char hostname[1024];
  hostname[1023] = '\0';
  gethostname(hostname, 1023);
  zkRegistrationName = lexical_cast<string>(hostname) + ":" + lexical_cast<string>(scribeHandlerPort);
  zkFullRegistrationName = zkRegistrationPrefix + "/" + zkRegistrationName;
  static string contents = "";
  struct Stat stat;
  char tmp[0];

  // Prefixs are created as regular znodes.
  if (ZOK != zoo_exists(zh, zkRegistrationPrefix.c_str(), 1, &stat)) {
    size_t index = zkRegistrationPrefix.find("/", 0);
    while (index < string::npos) {
      index = zkRegistrationPrefix.find("/", index+1);
      string prefix = zkRegistrationPrefix.substr(0, index);
      zoo_create(zh, prefix.c_str(), contents.c_str(), contents.length(),
          &ZOO_CREATOR_ALL_ACL, 0, tmp, sizeof(tmp));
    }
  }

  // Register this scribe as an ephemeral node.
  int ret = zoo_create(zh, zkFullRegistrationName.c_str(), contents.c_str(),
      contents.length(), &ZOO_CREATOR_ALL_ACL,
      ZOO_EPHEMERAL, tmp, sizeof(tmp));

  if (ZOK == ret) {
    return true;
  } else if (ZNODEEXISTS == ret) {
    if (ZOK == zoo_exists(zh, zkFullRegistrationName.c_str(), 1, &stat)) {
      LOG_DEBUG("Set watch on znode that already exists: %s", zkFullRegistrationName.c_str());
      return true;
    } else {
      LOG_OPER("Failed setting watch on znode: %s", zkFullRegistrationName.c_str());
      return false;
    }
  }
  LOG_OPER("Registration failed for unknown reason: %s", zkFullRegistrationName.c_str());
  return false;
}

bool ZKClient::updateStatus(std::string& current_status) {
  struct Stat stat;
  char tmp[0];
  int rc;
  if (zoo_exists(zh, zkFullRegistrationName.c_str(), 1, &stat) == ZOK) {
    rc = zoo_set(zh, zkFullRegistrationName.c_str(), current_status.c_str(),
                current_status.length() + 1, -1);
  } else {
    rc = zoo_create(zh, zkFullRegistrationName.c_str(), current_status.c_str(),
                    current_status.length() + 1, &ZOO_CREATOR_ALL_ACL,
                    ZOO_EPHEMERAL, tmp, sizeof(tmp));
  }
  if (rc) {
    LOG_OPER("Error %d for writing %s to ZK file %s", rc, current_status.c_str(), zkFullRegistrationName.c_str());
  } else {
    LOG_DEBUG("Write %s to ZK file %s", current_status.c_str(), zkFullRegistrationName.c_str());
  }
  return rc == 0;
}

// Get the best host:port to send messages to at this time.
bool ZKClient::getRemoteScribe(std::string& parentZnode,
    string& remoteHost,
    unsigned long& remotePort) {
  bool ret = false;
  bool should_disconnect = false;
  if (zkServer.empty()) {
    LOG_OPER("No zookeeper server! Unable to discover remote scribes.");
    return false;
  } else if (NULL == zh) {
    connect();
    should_disconnect = true;
  } else if (!zoo_state(zh)) {
    LOG_OPER("No zookeeper connection state! Unable to discover remote scribes.");
    return false;
  }
  LOG_DEBUG("Getting the best remote scribe.");
  struct String_vector children;
  if (zoo_get_children(zh, parentZnode.c_str(), 0, &children) != ZOK || children.count == 0) {
    LOG_OPER("Unable to discover remote scribes.");
    return false;
  }

  AggSelector *aggSelector = AggSelectorFactory::createAggSelector(zkAggSelectorKey);
  ret = aggSelector->selectScribeAggregator(children, remoteHost, remotePort);

  if (should_disconnect) {
    disconnect();
  }
  return ret;
}
