-- Tags: no-parallel
DROP DICTIONARY IF EXISTS d0;
DROP TABLE IF EXISTS t0;

CREATE TABLE t0 (
    key Int32,
    value Int32
)
ENGINE=MergeTree()
PRIMARY KEY key
PARTITION BY key % 2;

INSERT INTO t0 VALUES (0, 0);

CREATE DICTIONARY d0 (
    key Int32,
    value Int32
)
PRIMARY KEY key
SOURCE(CLICKHOUSE(DATABASE default TABLE t0))
LIFETIME(MIN 0 MAX 0)
LAYOUT(HASHED());

SELECT * FROM d0;

ALTER TABLE t0 ADD COLUMN key2 Int32 DEFAULT dictGetOrDefault('d0', 'value', 0, 1); -- {serverError INFINITE_LOOP}

DROP DICTIONARY d0;
DROP TABLE t0;
