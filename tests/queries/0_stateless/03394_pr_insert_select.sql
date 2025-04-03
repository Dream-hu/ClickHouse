-- Tags: distributed

SET parallel_distributed_insert_select=2;

-- MT

DROP TABLE IF EXISTS t_mt_source;
DROP TABLE IF EXISTS t_mt_target;

CREATE TABLE t_mt_source (k UInt64, v String) ENGINE = MergeTree() ORDER BY k;
CREATE TABLE t_mt_target (k UInt64, v String) ENGINE = MergeTree() ORDER BY ();

INSERT INTO t_mt_source SELECT number as k, toString(number) as v FROM system.numbers LIMIT 1e7;

SET enable_parallel_replicas = 1, parallel_replicas_for_non_replicated_merge_tree = 1, cluster_for_parallel_replicas = 'test_cluster_one_shard_three_replicas_localhost', max_parallel_replicas = 3;

INSERT INTO t_mt_target SELECT * FROM t_mt_source SETTINGS log_comment='cb01f13a-410c-4985-b233-35289776b58f';

SYSTEM FLUSH LOGS;
select count() from system.query_log where has(databases, currentDatabase()) and type = 'QueryFinish' and query_kind = 'Insert' and log_comment='cb01f13a-410c-4985-b233-35289776b58f' and event_date >= yesterday();

select count() from t_mt_source;
select count() from t_mt_target;

select * from t_mt_source order by k
except
select * from t_mt_target order by k;

DROP TABLE t_mt_source;
DROP TABLE t_mt_target;

-- RMT

DROP TABLE IF EXISTS t_rmt_source SYNC;
DROP TABLE IF EXISTS t_rmt_target SYNC;

CREATE TABLE t_rmt_source (k UInt64, v String) ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/t_rmt_source', 'r1') ORDER BY k;
CREATE TABLE t_rmt_target (k UInt64, v String) ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/t_rmt_target', 'r1') ORDER BY ();

INSERT INTO t_rmt_source SELECT number as k, toString(number) as v FROM system.numbers LIMIT 1_000_000 settings parallel_distributed_insert_select=0;

INSERT INTO t_rmt_target SELECT * FROM t_rmt_source SETTINGS log_comment='ac5dcf0f-c5a9-4e8c-9f17-e6c94093586f';

SYSTEM FLUSH LOGS;
select count() from system.query_log where has(databases, currentDatabase()) and type = 'QueryFinish' and query_kind = 'Insert' and log_comment='ac5dcf0f-c5a9-4e8c-9f17-e6c94093586f' and event_date >= yesterday();

select * from t_rmt_source order by k
except
select * from t_rmt_target order by k;

DROP TABLE t_rmt_source SYNC;
DROP TABLE t_rmt_target SYNC;
