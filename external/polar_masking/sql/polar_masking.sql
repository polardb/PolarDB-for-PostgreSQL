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

-- clear
drop table test1;
drop schema test_masking cascade;
drop extension polar_masking;
