create extension polar_masking;
show polar_masking.polar_masking_enabled;

create table test1(a int, b text, c varchar, d name);
create schema test_masking;
create table test_masking.test2(a int, b text, c varchar, d name);
create table test_masking.test3(a int, b text);
create table test_masking.test4(a int, b text);

-- label create/drop
select polar_masking.polar_masking_create_label(NULL);
select polar_masking.polar_masking_create_label('label1');
select polar_masking.polar_masking_create_label('label1');
select * from polar_masking.polar_masking_policy;
select polar_masking.polar_masking_create_label('Label2');
select * from polar_masking.polar_masking_policy;

select polar_masking.polar_masking_drop_label(NULL);
select polar_masking.polar_masking_drop_label('Label_2');
select polar_masking.polar_masking_drop_label('Label2');
select polar_masking.polar_masking_drop_label('Label2');
select * from polar_masking.polar_masking_policy;

-- label apply and remove test
select polar_masking.polar_masking_apply_label_to_table('Label2', NULL, 'test1');
select polar_masking.polar_masking_apply_label_to_table(NULL, 'public', 'test1');
select polar_masking.polar_masking_apply_label_to_table('Label2', 'public', NULL);
select polar_masking.polar_masking_apply_label_to_table('Label2', 'public', 'test1');
select polar_masking.polar_masking_apply_label_to_table('label1', 'abc', 'test1');
select polar_masking.polar_masking_apply_label_to_table('label1', 'public', 'test11');
select polar_masking.polar_masking_apply_label_to_table('label1', 'public', 'test1');
select * from polar_masking.polar_masking_label_tab;

select polar_masking.polar_masking_apply_label_to_table('label1', 'public', 'test1');
select polar_masking.polar_masking_apply_label_to_column('label1', 'public', 'test1', 'a');

select polar_masking.polar_masking_remove_table_from_label(NULL, 'public', 'test11');
select polar_masking.polar_masking_remove_table_from_label('label1', NULL, 'test11');
select polar_masking.polar_masking_remove_table_from_label('label1', 'public', NULL);
select polar_masking.polar_masking_remove_table_from_label('label1', 'public','test11');
select polar_masking.polar_masking_remove_table_from_label('label1', 'public','test1');
select * from polar_masking.polar_masking_label_tab;

select polar_masking.polar_masking_apply_label_to_column(NULL, 'public','test1', 'a');
select polar_masking.polar_masking_apply_label_to_column('label1', NULL,'test1', 'a');
select polar_masking.polar_masking_apply_label_to_column('label1', 'public', NULL, 'a');
select polar_masking.polar_masking_apply_label_to_column('label1', 'public','test1', NULL);
select polar_masking.polar_masking_apply_label_to_column('label1', 'public','test1', 'a');
select * from polar_masking.polar_masking_label_tab;
select * from polar_masking.polar_masking_label_col;

select polar_masking.polar_masking_apply_label_to_column('label1', 'public','test1', 'a');
select polar_masking.polar_masking_apply_label_to_column('label1', 'public','test1', 'b');
select * from polar_masking.polar_masking_label_col;

select polar_masking.polar_masking_apply_label_to_table('label1', 'public', 'test1');
select * from polar_masking.polar_masking_label_tab;

select polar_masking.polar_masking_remove_column_from_label('label1', 'public','test1', 'c');
select polar_masking.polar_masking_remove_column_from_label('label1', 'public','test11', 'c');
select polar_masking.polar_masking_remove_column_from_label('label1', 'public','test1', 'b');
select * from polar_masking.polar_masking_label_col;


select polar_masking.polar_masking_apply_label_to_column('label1', 'public','test1', 'b');
select polar_masking.polar_masking_apply_label_to_column('label1', 'public','test1', 'c');
select polar_masking.polar_masking_apply_label_to_table('label1', 'public','test2');
select polar_masking.polar_masking_apply_label_to_table('label1', 'test_masking','test2');
select polar_masking.polar_masking_apply_label_to_table('label1', 'test_masking','test3');
select polar_masking.polar_masking_create_label('label2');
select * from polar_masking.polar_masking_policy;
select polar_masking.polar_masking_apply_label_to_column('label2', 'public','test1', 'd');
select polar_masking.polar_masking_apply_label_to_table('label2', 'test_masking','test4');
select * from polar_masking.polar_masking_label_col;
select * from polar_masking.polar_masking_label_tab;

select polar_masking.polar_masking_drop_label('label1');

-- clear
drop table test1;
drop schema test_masking cascade;
drop extension polar_masking;
