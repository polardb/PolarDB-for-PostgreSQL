SET client_min_messages = warning;

SET ROLE polar_repack_superuser;

SET DATESTYLE TO ISO;

SELECT col1, to_char("time", 'YYYY-MM-DD HH24:MI:SS'), ","")" FROM tbl_cluster ORDER BY 1, 2;
SELECT * FROM tbl_only_ckey ORDER BY 1;
SELECT * FROM tbl_only_pkey ORDER BY 1;
SELECT * FROM tbl_incl_pkey ORDER BY 1;
SELECT * FROM tbl_gistkey ORDER BY 1;

SET enable_seqscan = on;
SET enable_indexscan = off;
SELECT * FROM tbl_with_dropped_column ORDER BY 1,2,3,4;
SELECT * FROM view_for_dropped_column ORDER BY 1, 2;
SELECT * FROM tbl_with_dropped_toast;
SET enable_seqscan = off;
SET enable_indexscan = on;
SELECT * FROM tbl_with_dropped_column ORDER BY 1, 2;
SELECT * FROM view_for_dropped_column ORDER BY 1,2,3,4;
SELECT * FROM tbl_with_dropped_toast;
RESET enable_seqscan;
RESET enable_indexscan;
-- check if storage option for both table and TOAST table didn't go away.
SELECT CASE relkind
       WHEN 'r' THEN relname
       WHEN 't' THEN 'toast_table'
       END as table,
       reloptions
FROM pg_class
WHERE relname = 'tbl_with_toast' OR relname = 'pg_toast_' || 'tbl_with_toast'::regclass::oid
ORDER BY 1;
SELECT pg_relation_size(reltoastrelid) = 0 as check_toast_rel_size FROM pg_class WHERE relname = 'tbl_with_mod_column_storage';

--
-- check broken links or orphan toast relations
--
SELECT oid, relname
  FROM pg_class
 WHERE relkind = 't'
   AND oid NOT IN (SELECT reltoastrelid FROM pg_class WHERE relkind = 'r');

SELECT oid, relname
  FROM pg_class
 WHERE relkind = 'r'
   AND reltoastrelid <> 0
   AND reltoastrelid NOT IN (SELECT oid FROM pg_class WHERE relkind = 't');

-- check columns options
SELECT attname, nullif(attstattarget, -1) as attstattarget,
       (SELECT array_agg(opt ORDER BY ord)
        FROM unnest(attoptions) WITH ORDINALITY AS opts(opt, ord)
        WHERE split_part(opt, '=', 1) <> 'pg_attver') AS attoptions
FROM pg_attribute
WHERE attrelid = 'tbl_idxopts'::regclass
AND attnum > 0
ORDER BY attnum;

--
-- NOT NULL UNIQUE
--
CREATE TABLE tbl_nn    (col1 int NOT NULL, col2 int NOT NULL);
CREATE TABLE tbl_uk    (col1 int NOT NULL, col2 int         , UNIQUE(col1, col2));
CREATE TABLE tbl_nn_uk (col1 int NOT NULL, col2 int NOT NULL, UNIQUE(col1, col2));
CREATE TABLE tbl_pk_uk (col1 int NOT NULL, col2 int NOT NULL, PRIMARY KEY(col1, col2), UNIQUE(col2, col1));
CREATE TABLE tbl_nn_puk (col1 int NOT NULL, col2 int NOT NULL);
CREATE UNIQUE INDEX tbl_nn_puk_pcol1_idx ON tbl_nn_puk(col1) WHERE col1 < 10;
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=tbl_nn
-- => WARNING
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=tbl_uk
-- => WARNING
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=tbl_nn_uk
-- => OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=tbl_pk_uk
-- => OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=tbl_pk_uk --only-indexes
-- => OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=tbl_nn_puk
-- => WARNING

--
-- Triggers handling
--
CREATE FUNCTION trgtest() RETURNS trigger AS
$$BEGIN RETURN NEW; END$$
LANGUAGE plpgsql;
CREATE TABLE trg1 (id integer PRIMARY KEY);
CREATE TRIGGER repack_trigger_1 AFTER UPDATE ON trg1 FOR EACH ROW EXECUTE PROCEDURE trgtest();
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=trg1
CREATE TABLE trg2 (id integer PRIMARY KEY);
CREATE TRIGGER repack_trigger AFTER UPDATE ON trg2 FOR EACH ROW EXECUTE PROCEDURE trgtest();
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=trg2
CREATE TABLE trg3 (id integer PRIMARY KEY);
CREATE TRIGGER repack_trigger_1 BEFORE UPDATE ON trg3 FOR EACH ROW EXECUTE PROCEDURE trgtest();
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=trg3

--
-- Table re-organization using specific column
--

-- reorganize table using cluster key. Sort in ascending order.
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=tbl_order
SELECT ctid, c FROM tbl_order WHERE ctid <= '(0,10)';

-- reorganize table using specific column order. Sort in descending order.
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=tbl_order -o "c DESC"
SELECT ctid, c FROM tbl_order WHERE ctid <= '(0,10)';


--
-- Dry run
--
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=tbl_cluster --dry-run

-- Test --schema
--
CREATE SCHEMA test_schema1;
CREATE TABLE test_schema1.tbl1 (id INTEGER PRIMARY KEY);
CREATE TABLE test_schema1.tbl2 (id INTEGER PRIMARY KEY);
CREATE SCHEMA test_schema2;
CREATE TABLE test_schema2.tbl1 (id INTEGER PRIMARY KEY);
CREATE TABLE test_schema2.tbl2 (id INTEGER PRIMARY KEY);
-- => OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --schema=test_schema1 --polar-no-disable
-- => OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --schema=test_schema1 --schema=test_schema2 --polar-no-disable
-- => ERROR
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --schema=test_schema1 --table=tbl1 --polar-no-disable
-- => ERROR
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --all --schema=test_schema1 --polar-no-disable

--
-- don't kill backend
--
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=tbl_cluster --no-kill-backend

--
-- exclude extension check
--
CREATE SCHEMA exclude_extension_schema;
CREATE TABLE exclude_extension_schema.tbl(val integer primary key);
-- => ERROR
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=dummy_table --exclude-extension=dummy_extension
-- => ERROR
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=dummy_table --exclude-extension=dummy_extension -x
-- => ERROR
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=dummy_index --exclude-extension=dummy_extension
-- => OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --schema=exclude_extension_schema --exclude-extension=dummy_extension --polar-no-disable
-- => OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --schema=exclude_extension_schema --exclude-extension=dummy_extension --exclude-extension=dummy_extension --polar-no-disable

--
-- table inheritance check
--
CREATE TABLE parent_a(val integer primary key);
CREATE TABLE child_a_1(val integer primary key) INHERITS(parent_a);
CREATE TABLE child_a_2(val integer primary key) INHERITS(parent_a);
CREATE TABLE parent_b(val integer primary key);
CREATE TABLE child_b_1(val integer primary key) INHERITS(parent_b);
CREATE TABLE child_b_2(val integer primary key) INHERITS(parent_b);
-- => ERROR
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=dummy_table
-- => ERROR
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=dummy_index --index=dummy_index
-- => ERROR
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=dummy_table --schema=dummy_schema --polar-no-disable
-- => ERROR
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=dummy_table --all --polar-no-disable
-- => OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=parent_a --parent-table=parent_b
-- => OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=parent_a --parent-table=parent_b
-- => OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=parent_a --parent-table=parent_b --only-indexes
-- => OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=parent_a --parent-table=parent_b --only-indexes

--
-- Apply count
--
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=tbl_cluster --apply-count 1234
--
-- Switch threshold
--
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=tbl_cluster --switch-threshold 200

--
-- partitioned table check
--
CREATE TABLE partitioned_a(val integer NOT NULL) PARTITION BY RANGE (val);
CREATE TABLE partition_a_1 PARTITION OF partitioned_a FOR VALUES FROM (0) TO (1000);
CREATE TABLE partition_a_2 PARTITION OF partitioned_a FOR VALUES FROM (1000) TO (2000);
-- Create indexes separately to support Postgres 10 which doesn't support
-- indexes on a partitioned table itself
CREATE UNIQUE INDEX partition_a_1_val_idx ON partition_a_1 (val);
CREATE UNIQUE INDEX partition_a_2_val_idx ON partition_a_2 (val);
-- These statements will fail on Postgres 10
CREATE UNIQUE INDEX partitioned_a_val_idx ON ONLY partitioned_a (val);
ALTER INDEX partitioned_a_val_idx ATTACH PARTITION partition_a_1_val_idx;
ALTER INDEX partitioned_a_val_idx ATTACH PARTITION partition_a_2_val_idx;
-- => OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=partitioned_a
-- => OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=partitioned_a --only-indexes
-- => OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=partitioned_a --parent-table=parent_a
-- => OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=partitioned_a --parent-table=parent_a --only-indexes
