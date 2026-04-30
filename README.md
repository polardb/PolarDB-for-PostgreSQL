<div align="center">

[![logo](https://raw.githubusercontent.com/polardb/polardb-pg-docs/main/docs/.vuepress/public/images/polardb.png)](https://polardb.github.io/polardb-pg-docs/)

# PolarDB for PostgreSQL

**A cloud-native database developed by Alibaba Cloud**

#### English | [简体中文](./README_zh.md)

[![official](https://img.shields.io/badge/official%20site-blueviolet?style=flat&logo=alibabacloud)](https://polardb.github.io/polardb-pg-docs/)

[![GitHub License](https://img.shields.io/github/license/polardb/PolarDB-for-PostgreSQL?style=flat&logo=apache)](./LICENSE)
[![github-issues](https://img.shields.io/github/issues/polardb/PolarDB-for-PostgreSQL?style=flat&logo=github)](https://github.com/polardb/PolarDB-for-PostgreSQL/issues)
[![github-pullrequest](https://img.shields.io/github/issues-pr/polardb/PolarDB-for-PostgreSQL?style=flat&logo=github)](https://github.com/polardb/PolarDB-for-PostgreSQL/pulls)
[![GitHub Discussions](https://img.shields.io/github/discussions/polardb/PolarDB-for-PostgreSQL?logo=github)](https://github.com/polardb/PolarDB-for-PostgreSQL/discussions)
[![github-forks](https://img.shields.io/github/forks/polardb/PolarDB-for-PostgreSQL?style=flat&logo=github)](https://github.com/polardb/PolarDB-for-PostgreSQL/network/members)
[![github-stars](https://img.shields.io/github/stars/polardb/PolarDB-for-PostgreSQL?style=flat&logo=github)](https://github.com/polardb/PolarDB-for-PostgreSQL/stargazers)
[![github-contributors](https://img.shields.io/github/contributors/polardb/PolarDB-for-PostgreSQL?style=flat&logo=github)](https://github.com/polardb/PolarDB-for-PostgreSQL/graphs/contributors)
[![GitHub deployments](<https://img.shields.io/github/deployments/polardb/polardb-pg-docs/github-pages?logo=github&label=github-pages%20(docs)>)](https://polardb.github.io/polardb-pg-docs/zh/)
[![GitHub branch check runs](<https://img.shields.io/github/check-runs/polardb/PolarDB-for-PostgreSQL/POLARDB_15_STABLE?logo=github&label=checks%20(v15)>)](https://github.com/polardb/PolarDB-for-PostgreSQL/tree/POLARDB_15_STABLE)
[![Leaderboard](https://img.shields.io/badge/PolarDB--for--PostgreSQL-Check%20Your%20Contribution-orange?style=flat&logo=alibabacloud)](https://opensource.alibaba.com/contribution_leaderboard/details?projectValue=polardb-pg)

</div>

## Overview

![arch.png](https://raw.githubusercontent.com/polardb/polardb-pg-docs/main/docs/imgs/1_polardb_architecture.png)

PolarDB for PostgreSQL (hereafter simplified as PolarDB) is a cloud native database service independently developed by Alibaba Cloud. This service is 100% compatible with PostgreSQL and uses a shared-storage-based architecture in which computing is decoupled from storage. This service features flexible scalability, millisecond-level latency and hybrid transactional/analytical processing (HTAP) capabilities.

1. Flexible scalability: You can use the service to scale out a compute cluster or a storage cluster based on your business requirements.
   - If the computing power is insufficient, you can scale out only the compute cluster.
   - If the storage capacity or the storage I/O is insufficient, you can scale out a storage cluster without interrupting your service.
2. Millisecond-level latency:
   - Write-ahead logging (WAL) logs are stored in the shared storage. Only the metadata of WAL records is replicated from the read-write node to read-only nodes.
   - The _LogIndex_ technology provided by PolarDB features two record replay modes: lazy replay and parallel replay. The technology can be used to minimize the record replication latency from the read-write node to read-only nodes.
3. HTAP: HTAP is implemented by using a shared-storage-based massively parallel processing (MPP) architecture. The architecture is used to accelerate online analytical processing (OLAP) queries in online transaction processing (OLTP) scenarios. PolarDB supports a complete suite of data types that are used in OLTP scenarios. PolarDB supports two computing engines that can process these types of data:
   - Standalone execution: processes OLTP queries that feature high concurrency.
   - Distributed execution: processes large OLAP queries.

PolarDB provides a wide range of innovative multi-model database capabilities to help you process, analyze, and search for different types of data, such as spatio-temporal, geographic information system (GIS), image, vector, and graph data.

## Branch Introduction

The `POLARDB_15_STABLE` is the stable branch based on PostgreSQL 15, which supports compute-storage separation architecture.

## Architecture and Roadmap

PolarDB for PostgreSQL uses a shared-storage-based architecture in which computing is decoupled from storage. The conventional shared-nothing architecture is changed to the shared-storage architecture. N copies of data in the compute cluster and N copies of data in the storage cluster are changed to N copies of data in the compute cluster and one copy of data in the storage cluster. The shared storage stores one copy of data, but the data states in memory are different. The WAL logs must be synchronized from the primary node to read-only nodes to ensure data consistency. In addition, when the primary node flushes dirty pages, it must be controlled to prevent the read-only nodes from reading future pages. Meanwhile, the read-only nodes must be prevented from reading the outdated pages that are not correctly replayed in memory. To resolve this issue, PolarDB provides the index structure _LogIndex_ to maintain the page replay history. LogIndex can be used to synchronize data from the primary node to read-only nodes.

After computing is decoupled from storage, the I/O latency and throughput increase. When a single read-only node is used to process analytical queries, the CPUs, memory, and I/O of other read-only nodes and the large storage I/O bandwidth cannot be fully utilized. To resolve this issue, PolarDB provides the shared-storage-based MPP engine. The engine can use CPUs to accelerate analytical queries at SQL level and support a mix of OLAP workloads and OLTP workloads for HTAP.

For more information, see [Architecture](https://polardb.github.io/polardb-pg-docs/theory/arch-overview.html).

## Quick Start with PolarDB

If you have Docker installed already，then you can pull the instance image of PolarDB-PG based on local storage. Create, run and enter the container, and use PolarDB-PG instance directly:

```bash
# pull the instance image and run the container
docker pull polardb/polardb_pg_local_instance:15
docker run -it --cap-add=SYS_PTRACE --privileged=true --rm polardb/polardb_pg_local_instance:15 psql
# check
postgres=# SELECT version();
                                   version
----------------------------------------------------------------------
 PostgreSQL 15.x (PolarDB 15.x.x.x build xxxxxxxx) on {your_platform}
(1 row)
```

For more advanced deployment way, please refer to [Advanced Deployment](https://polardb.github.io/polardb-pg-docs/deploying/deploy.html). Before your deployment, we recommand to figure out the [Architecture](https://polardb.github.io/polardb-pg-docs/deploying/introduction.html) of PolarDB for PostgreSQL.

## Development

Please refer to [Development Guide](https://polardb.github.io/polardb-pg-docs/development/dev-on-docker.html) to compile and development PolarDB for PostgreSQL.

## Documentation

Please refer to [Online Documentation Website](https://polardb.github.io/polardb-pg-docs/) to see the whole documentations.

If you want to explore or develop documentation locally, see [Document Contribution](https://polardb.github.io/polardb-pg-docs/contributing/contributing-polardb-docs.html).

## Contributing

You are welcome to make contributions to PolarDB for PostgreSQL. Here are the contributors:

<a href="https://github.com/polardb/PolarDB-for-PostgreSQL/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=polardb/PolarDB-for-PostgreSQL" />
</a>

Made with [contrib.rocks](https://contrib.rocks).

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=polardb/PolarDB-for-PostgreSQL&type=Date)](https://star-history.com/#polardb/PolarDB-for-PostgreSQL&Date)

## Software License

PolarDB for PostgreSQL is released under Apache License (Version 2.0), developed based on PostgreSQL which is released under PostgreSQL License. See [LICENSE](./LICENSE) for more information.

PolarDB for PostgreSQL contains various third-party components under other open source licenses. See [NOTICE](./NOTICE) for more information.

## Join the Community

Click this [link](https://qr.dingtalk.com/action/joingroup?code=v1,k1,AEOqzSc8Uwzer7yhpNeYp8okNX3KVqpDMk/2oZ3ZRnQ=&_dt_no_comment=1&origin=11) and use DingTalk application to join the DingTalk group.

## Copyright

Copyright © Alibaba Group, Inc.
