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

-- masking operators test
drop table test1;
create table test1(a int, b text, c varchar, d name, e bpchar, f char);

insert into test1 values (1,'1000-1111-1111-0011', 'abc-1111-1111-0011', 'abc-1111-1111-@1aa', 'abbcc-xx-addd-@aa.a','a');
insert into test1 values (2,'1111111@aaaa.com', 'aaaabbbb@aaaa.com', 'ccccccaabbbb@a123a.com', 'd@a123.com','b');
insert into test1 values (3,'1111111@aaaa.com', '1abc-1111-1d11-0d011', '111111111', 'abcdefg','4');
insert into test1 values (4,'1111111@aaaa.com', '1abc-1111-1d11-0d011', '123456789', 'abcdefg123','c');
insert into test1 values (5,'1111111@aaaa.com', '1abc-1111-1d11-0d011', '123456789', 'abcdefg123','c');

select * from test1;

select polar_masking.polar_masking_create_label('label1');
select polar_masking.polar_masking_apply_label_to_table('label1', 'public','test1');
select polar_masking.polar_masking_alter_label_maskingop('label1',  'creditcardmasking');
select * from test1;
select polar_masking.polar_masking_alter_label_maskingop('label1',  'basicemailmasking');
select * from test1;
select polar_masking.polar_masking_alter_label_maskingop('label1',  'fullemailmasking');
select * from test1;
select polar_masking.polar_masking_alter_label_maskingop('label1',  'alldigitsmasking');
select * from test1;
select polar_masking.polar_masking_alter_label_maskingop('label1',  'shufflemasking');
select * from polar_masking.polar_masking_policy;
select polar_masking.polar_masking_alter_label_maskingop('label1',  'randommasking');
select * from polar_masking.polar_masking_policy;
select polar_masking.polar_masking_alter_label_maskingop('label1',  'maskall');
select * from test1;
select polar_masking.polar_masking_alter_label_maskingop_set_regexpmasking('label1', 5, 8, '[\d+]', 'z');
select * from polar_masking.polar_masking_policy;
select * from polar_masking.polar_masking_policy_regex;
select * from test1;

select polar_masking.polar_masking_alter_label_maskingop_set_regexpmasking('label1', 2, 12, '[\d+]', 'z');
select * from polar_masking.polar_masking_policy;
select * from polar_masking.polar_masking_policy_regex;
select * from test1;

select polar_masking.polar_masking_alter_label_maskingop('label1',  'creditcardmasking');
select * from polar_masking.polar_masking_policy;
select * from polar_masking.polar_masking_policy_regex;
select * from test1;

select polar_masking.polar_masking_alter_label_maskingop_set_regexpmasking('label1', -1, 12, '[\d+]', 'z');
select polar_masking.polar_masking_alter_label_maskingop_set_regexpmasking('label1', 1, -12, '[\d+]', 'z');
select polar_masking.polar_masking_alter_label_maskingop_set_regexpmasking('label1', 8, 4, '[\d+]', 'z');

select polar_masking.polar_masking_alter_label_maskingop_set_regexpmasking('label1', 0, 0, '[\d+]', 'z');
select * from polar_masking.polar_masking_policy;
select * from polar_masking.polar_masking_policy_regex;
select * from test1;

select polar_masking.polar_masking_alter_label_maskingop_set_regexpmasking('label1', 0, 3, '[\d+]', 'z');
select * from polar_masking.polar_masking_policy;
select * from polar_masking.polar_masking_policy_regex;
select * from test1;

select polar_masking.polar_masking_alter_label_maskingop_set_regexpmasking('label1', 10, 0, '[\d+]', 'z');
select * from polar_masking.polar_masking_policy;
select * from polar_masking.polar_masking_policy_regex;
select * from test1;

create view test_view as select * from test1;
select * from test_view;
select polar_masking.polar_masking_alter_label_maskingop_set_regexpmasking('label1', 0, 0, '[\d+]', 'z');
select * from test_view;

select polar_masking.polar_masking_alter_label_maskingop('label1',  'none');
select * from test_view;
select * from test1;
select * from polar_masking.polar_masking_policy;
select * from polar_masking.polar_masking_policy_regex;

select polar_masking.polar_masking_remove_table_from_label('label1', 'public','test1');
select polar_masking.polar_masking_apply_label_to_column('label1', 'public','test1', 'b');
select polar_masking.polar_masking_apply_label_to_column('label2', 'public','test1', 'c');

select polar_masking.polar_masking_create_label('label3');
select polar_masking.polar_masking_create_label('label4');
select polar_masking.polar_masking_apply_label_to_column('label3', 'public','test1', 'd');
select polar_masking.polar_masking_apply_label_to_column('label4', 'public','test1', 'e');

select  polar_masking.polar_masking_alter_label_maskingop('label1',  'shufflemasking');
select  polar_masking.polar_masking_alter_label_maskingop('label2',  'randommasking');
select  polar_masking.polar_masking_alter_label_maskingop_set_regexpmasking('label3', 5, 8, '[\d+]', 'abc');
select  polar_masking.polar_masking_alter_label_maskingop('label4',  'maskall');
select * from polar_masking.polar_masking_policy;
select * from polar_masking.polar_masking_policy_regex;
select a,d,e,f from test1;

select  polar_masking.polar_masking_alter_label_maskingop('label1',  'creditcardmasking');
select  polar_masking.polar_masking_alter_label_maskingop('label2',  'basicemailmasking');
select  polar_masking.polar_masking_alter_label_maskingop('label3',  'fullemailmasking');
select  polar_masking.polar_masking_alter_label_maskingop('label4',  'alldigitsmasking');
select * from polar_masking.polar_masking_policy;
select * from polar_masking.polar_masking_policy_regex;
select * from test1;
select * from test1 where b = '1111111@aaaa.com';
select * from test1 where c = '1abc-1111-1d11-0d011';
select * from test1 where d = '123456789';
select * from test1 where e = 'abcdefg123';

-- clear
drop view test_view;
drop table test1;
drop schema test_masking cascade;
drop extension polar_masking;
