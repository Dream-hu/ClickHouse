DROP TABLE IF EXISTS dst;
DROP TABLE IF EXISTS src;
DROP VIEW IF EXISTS v;
CREATE TABLE dst (c0 Int, c1 Int) ENGINE = MergeTree() ORDER BY tuple();
CREATE TABLE src (c0 Nullable(Int), c1 Int) ENGINE = MergeTree() ORDER BY tuple();
CREATE MATERIALIZED VIEW v TO dst (c0 Nullable(Int), c1 Int) AS (SELECT c0, c1 from src);
INSERT INTO TABLE src (c0, c1) VALUES (1, 1);
SELECT * FROM v PREWHERE c0 = 1; -- {serverError ILLEGAL_PREWHERE}
SELECT * FROM v PREWHERE c1 = 1;
SELECT * FROM v PREWHERE c0 = c1; -- {serverError ILLEGAL_PREWHERE}
DROP VIEW v;
DROP TABLE src;
DROP TABLE dst;

