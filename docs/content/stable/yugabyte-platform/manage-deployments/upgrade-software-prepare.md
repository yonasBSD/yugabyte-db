---
title: Prepare to upgrade universes with a new version of YugabyteDB
headerTitle: Prepare to upgrade a universe
linkTitle: Prepare to upgrade
description: Use YugabyteDB Anywhere to upgrade the YugabyteDB software on universes.
headcontent: Review changes that may affect your automation
menu:
  stable_yugabyte-platform:
    identifier: upgrade-software-prepare
    parent: upgrade-software
    weight: 10
type: docs
---

## Review major changes in previous YugabyteDB releases

Before starting the upgrade, review the following major changes in previous YugabyteDB releases. Depending on the upgrade you are planning, you may need to make changes to your automation.

### Upgrading from versions earlier than v2.16.0

The YB Controller (YBC) service was introduced in v2.16.0 for all universes (except Kubernetes), and is required for YBA 2.16.0 and later.

YBC is used to manage backup and restore, providing faster full backups, and introduces support for incremental backups.

**Impacts**

- Firewall ports - update your firewall rules to allow incoming TCP traffic on port 18018, which is used by YBC, for all nodes in a universe.

- On-premises provider - if you use on-premises providers with manually-provisioned nodes, update your current procedures for manually provisioning instances to accommodate YBC. This includes the following:

  - Set systemd-specific database service unit files (if used). Refer to [Manually provision on-premises nodes](../../configure-yugabyte-platform/set-up-cloud-provider/on-premises-manual/).

  - After upgrading nodes, manually install YBC on the nodes. Refer to [Upgrade manually-provisioned on-premises universe](#upgrade-manually-provisioned-on-premises-universe).

- OS patching procedure - for universes created using an on-premises provider with manually-provisioned nodes, if your OS patching procedures involve re-installing YugabyteDB software on a node, you will need to update those procedures to accommodate YBC.

### Upgrading from versions earlier than v2.18.0

YBC was introduced for Kubernetes clusters in v2.18.0. Refer to [Upgrading from versions earlier than v2.16.0](#upgrading-from-versions-earlier-than-v2160).

### Upgrading from versions earlier than v2.18.2

The Node Agent was introduced for all universes in v2.18.2. Node agent is an RPC service running on a YugabyteDB node, and is used to manage communication between YBA and the nodes in universes. Except for Day 0 tasks during initial installation, YBA no longer uses SSH and SCP to manage nodes; instead, YBA connects to the Node agent process listening on port 9070, and performs all its management via this secure connection. For more information, refer to the [Node agent FAQ](../../../faq/yugabyte-platform/#node-agent).

**Impacts**

- Firewall ports - update your firewall rules to allow incoming TCP traffic on port 9070 for all nodes in a universe.

- On-premises provider - if you use on-premises providers with manually-provisioned nodes, you will need to update your current procedures for manually provisioning instances to include installing node agent. Refer to [Install node agent](../../configure-yugabyte-platform/set-up-cloud-provider/on-premises-manual/#install-node-agent).

- OS patching procedure - for universes created using an on-premises provider with manually-provisioned nodes, if your OS patching procedures involve re-installing YugabyteDB software on a node, you will need to update those procedures to accommodate YBC.