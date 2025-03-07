-- Tags: stateful, no-parallel-replicas
-- Requires investigation (max_bytes_to_read is not respected)

SET max_bytes_to_read = 600000000;

SET optimize_move_to_prewhere = 1;
SET enable_multiple_prewhere_read_steps = 1;

SELECT uniq(URL) FROM test.hits WHERE toTimeZone(EventTime, 'Asia/Dubai') >= '2014-03-20 00:00:00' AND toTimeZone(EventTime, 'Asia/Dubai') < '2014-03-21 00:00:00';
SELECT uniq(URL) FROM test.hits WHERE toTimeZone(EventTime, 'Asia/Dubai') >= '2014-03-20 00:00:00' AND toTimeZone(EventTime, 'Asia/Dubai') < '2014-03-21 00:00:00' AND URL != '';
SELECT uniq(*) FROM test.hits WHERE toTimeZone(EventTime, 'Asia/Dubai') >= '2014-03-20 00:00:00' AND toTimeZone(EventTime, 'Asia/Dubai') < '2014-03-21 00:00:00' AND EventDate = '2014-03-21';
WITH toTimeZone(EventTime, 'Asia/Dubai') AS xyz SELECT uniq(*) FROM test.hits WHERE xyz >= '2014-03-20 00:00:00' AND xyz < '2014-03-21 00:00:00' AND EventDate = '2014-03-21';

SET optimize_move_to_prewhere = 0;
SET enable_multiple_prewhere_read_steps = 0;

SELECT uniq(URL) FROM test.hits WHERE toTimeZone(EventTime, 'Asia/Dubai') >= '2014-03-20 00:00:00' AND toTimeZone(EventTime, 'Asia/Dubai') < '2014-03-21 00:00:00'; -- { serverError TOO_MANY_BYTES }
SELECT uniq(URL) FROM test.hits WHERE toTimeZone(EventTime, 'Asia/Dubai') >= '2014-03-20 00:00:00' AND URL != '' AND toTimeZone(EventTime, 'Asia/Dubai') < '2014-03-21 00:00:00'; -- { serverError TOO_MANY_BYTES }
SELECT uniq(URL) FROM test.hits PREWHERE toTimeZone(EventTime, 'Asia/Dubai') >= '2014-03-20 00:00:00' AND URL != '' AND toTimeZone(EventTime, 'Asia/Dubai') < '2014-03-21 00:00:00'; -- { serverError TOO_MANY_BYTES }
