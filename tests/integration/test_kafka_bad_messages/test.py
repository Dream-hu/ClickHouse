import logging
import time

import pytest
from kafka import BrokerConnection, KafkaAdminClient, KafkaConsumer, KafkaProducer
from kafka.admin import NewTopic

from helpers.cluster import ClickHouseCluster, is_arm

if is_arm():
    pytestmark = pytest.mark.skip


cluster = ClickHouseCluster(__file__)
instance = cluster.add_instance(
    "instance",
    main_configs=["configs/kafka.xml"],
    with_kafka=True,
)


def kafka_create_topic(
    admin_client,
    topic_name,
    num_partitions=1,
    replication_factor=1,
    max_retries=50,
    config=None,
):
    logging.debug(
        f"Kafka create topic={topic_name}, num_partitions={num_partitions}, replication_factor={replication_factor}"
    )
    topics_list = [
        NewTopic(
            name=topic_name,
            num_partitions=num_partitions,
            replication_factor=replication_factor,
            topic_configs=config,
        )
    ]
    retries = 0
    while True:
        try:
            admin_client.create_topics(new_topics=topics_list, validate_only=False)
            logging.debug("Admin client succeed")
            return
        except Exception as e:
            retries += 1
            time.sleep(0.5)
            if retries < max_retries:
                logging.warning(f"Failed to create topic {e}")
            else:
                raise


def kafka_delete_topic(admin_client, topic, max_retries=50):
    result = admin_client.delete_topics([topic])
    for topic, e in result.topic_error_codes:
        if e == 0:
            logging.debug(f"Topic {topic} deleted")
        else:
            logging.error(f"Failed to delete topic {topic}: {e}")

    retries = 0
    while True:
        topics_listed = admin_client.list_topics()
        logging.debug(f"TOPICS LISTED: {topics_listed}")
        if topic not in topics_listed:
            return
        else:
            retries += 1
            time.sleep(0.5)
            if retries > max_retries:
                raise Exception(f"Failed to delete topics {topic}, {result}")


def get_kafka_producer(port, serializer, retries):
    errors = []
    for _ in range(retries):
        try:
            producer = KafkaProducer(
                bootstrap_servers="localhost:{}".format(port),
                value_serializer=serializer,
            )
            logging.debug("Kafka Connection establised: localhost:{}".format(port))
            return producer
        except Exception as e:
            errors += [str(e)]
            time.sleep(1)

    raise Exception("Connection not establised, {}".format(errors))


def producer_serializer(x):
    return x.encode() if isinstance(x, str) else x


def kafka_produce(
    kafka_cluster, topic, messages, timestamp=None, retries=15, partition=None
):
    logging.debug(
        "kafka_produce server:{}:{} topic:{}".format(
            "localhost", kafka_cluster.kafka_port, topic
        )
    )
    producer = get_kafka_producer(
        kafka_cluster.kafka_port, producer_serializer, retries
    )
    for message in messages:
        producer.send(
            topic=topic, value=message, timestamp_ms=timestamp, partition=partition
        )
        producer.flush()


@pytest.fixture(scope="module")
def kafka_cluster():
    try:
        cluster.start()
        kafka_id = instance.cluster.kafka_docker_id
        print(("kafka_id is {}".format(kafka_id)))
        yield cluster
    finally:
        cluster.shutdown()


def test_bad_messages_parsing_stream(kafka_cluster):
    admin_client = KafkaAdminClient(
        bootstrap_servers="localhost:{}".format(kafka_cluster.kafka_port)
    )

    for format_name in [
        "TSV",
        "TSKV",
        "CSV",
        "Values",
        "JSON",
        "JSONEachRow",
        "JSONCompactEachRow",
        "JSONObjectEachRow",
        "Avro",
        "RowBinary",
        "JSONColumns",
        "JSONColumnsWithMetadata",
        "Native",
        "Arrow",
        "ArrowStream",
        "Parquet",
        "ORC",
        "JSONCompactColumns",
        "BSONEachRow",
        "MySQLDump",
    ]:
        print(format_name)

        kafka_create_topic(admin_client, f"{format_name}_err")

        instance.query(
            f"""
            DROP TABLE IF EXISTS view;
            DROP TABLE IF EXISTS kafka;

            CREATE TABLE kafka (key UInt64, value UInt64)
                ENGINE = Kafka
                SETTINGS kafka_broker_list = 'kafka1:19092',
                         kafka_topic_list = '{format_name}_err',
                         kafka_group_name = '{format_name}',
                         kafka_format = '{format_name}',
                         kafka_flush_interval_ms=1000,
                         kafka_handle_error_mode='stream';

            CREATE MATERIALIZED VIEW view Engine=Log AS
                SELECT _error FROM kafka WHERE length(_error) != 0 ;
        """
        )

        messages = ["qwertyuiop", "asdfghjkl", "zxcvbnm"]
        kafka_produce(kafka_cluster, f"{format_name}_err", messages)

        attempt = 0
        rows = 0
        while attempt < 500:
            rows = int(instance.query("SELECT count() FROM view"))
            if rows == len(messages):
                break
            attempt += 1

        assert rows == len(messages)

        kafka_delete_topic(admin_client, f"{format_name}_err")

    protobuf_schema = """
syntax = "proto3";

message Message {
  uint64 key = 1;
  uint64 value = 2;
};
"""

    instance.create_format_schema("schema_test_errors.proto", protobuf_schema)

    for format_name in ["Protobuf", "ProtobufSingle", "ProtobufList"]:
        instance.query(
            f"""
            DROP TABLE IF EXISTS view;
            DROP TABLE IF EXISTS kafka;

            CREATE TABLE kafka (key UInt64, value UInt64)
                ENGINE = Kafka
                SETTINGS kafka_broker_list = 'kafka1:19092',
                         kafka_topic_list = '{format_name}_err',
                         kafka_group_name = '{format_name}',
                         kafka_format = '{format_name}',
                         kafka_handle_error_mode='stream',
                         kafka_flush_interval_ms=1000,
                         kafka_schema='schema_test_errors:Message';

            CREATE MATERIALIZED VIEW view Engine=Log AS
                SELECT _error FROM kafka WHERE length(_error) != 0 ;
        """
        )

        print(format_name)

        kafka_create_topic(admin_client, f"{format_name}_err")

        messages = ["qwertyuiop", "poiuytrewq", "zxcvbnm"]
        kafka_produce(kafka_cluster, f"{format_name}_err", messages)

        attempt = 0
        rows = 0
        while attempt < 500:
            rows = int(instance.query("SELECT count() FROM view"))
            if rows == len(messages):
                break
            attempt += 1

        assert rows == len(messages)

        kafka_delete_topic(admin_client, f"{format_name}_err")

    capn_proto_schema = """
@0xd9dd7b35452d1c4f;

struct Message
{
    key @0 : UInt64;
    value @1 : UInt64;
}
"""

    instance.create_format_schema("schema_test_errors.capnp", capn_proto_schema)
    instance.query(
        f"""
            DROP TABLE IF EXISTS view;
            DROP TABLE IF EXISTS kafka;

            CREATE TABLE kafka (key UInt64, value UInt64)
                ENGINE = Kafka
                SETTINGS kafka_broker_list = 'kafka1:19092',
                         kafka_topic_list = 'CapnProto_err',
                         kafka_group_name = 'CapnProto',
                         kafka_format = 'CapnProto',
                         kafka_handle_error_mode='stream',
                         kafka_flush_interval_ms=1000,
                         kafka_schema='schema_test_errors:Message';

            CREATE MATERIALIZED VIEW view Engine=Log AS
                SELECT _error FROM kafka WHERE length(_error) != 0;
        """
    )

    print("CapnProto")

    kafka_create_topic(admin_client, "CapnProto_err")

    messages = ["qwertyuiop", "asdfghjkl", "zxcvbnm"]
    kafka_produce(kafka_cluster, "CapnProto_err", messages)

    attempt = 0
    rows = 0
    while attempt < 500:
        rows = int(instance.query("SELECT count() FROM view"))
        if rows == len(messages):
            break
        attempt += 1

    assert rows == len(messages)

    kafka_delete_topic(admin_client, "CapnProto_err")


def test_bad_messages_parsing_exception(kafka_cluster, max_retries=20):
    admin_client = KafkaAdminClient(
        bootstrap_servers="localhost:{}".format(kafka_cluster.kafka_port)
    )

    for format_name in [
        "Avro",
        "JSONEachRow",
    ]:
        print(format_name)

        kafka_create_topic(admin_client, f"{format_name}_parsing_err")

        instance.query(
            f"""
            DROP TABLE IF EXISTS view_{format_name};
            DROP TABLE IF EXISTS kafka_{format_name};
            DROP TABLE IF EXISTS kafka;

            CREATE TABLE kafka_{format_name} (key UInt64, value UInt64)
                ENGINE = Kafka
                SETTINGS kafka_broker_list = 'kafka1:19092',
                         kafka_topic_list = '{format_name}_parsing_err',
                         kafka_group_name = '{format_name}',
                         kafka_format = '{format_name}',
                         kafka_flush_interval_ms=1000,
                         kafka_num_consumers = 1;

            CREATE MATERIALIZED VIEW view_{format_name} Engine=Log AS
                SELECT * FROM kafka_{format_name};
        """
        )

        kafka_produce(
            kafka_cluster,
            f"{format_name}_parsing_err",
            ["qwertyuiop", "asdfghjkl", "zxcvbnm"],
        )

    expected_result = """avro::Exception: Invalid data file. Magic does not match: : while parsing Kafka message (topic: Avro_parsing_err, partition: 0, offset: 0)\\'|1|1|1|default|kafka_Avro
Cannot parse input: expected \\'{\\' before: \\'qwertyuiop\\': (at row 1)\\n: while parsing Kafka message (topic: JSONEachRow_parsing_err, partition:|1|1|1|default|kafka_JSONEachRow
"""
    # filter out stacktrace in exceptions.text[1] because it is hardly stable enough
    result_system_kafka_consumers = instance.query_with_retry(
        """
        SELECT substr(exceptions.text[1], 1, 139), length(exceptions.text) > 1 AND length(exceptions.text) < 15, length(exceptions.time) > 1 AND length(exceptions.time) < 15, abs(dateDiff('second', exceptions.time[1], now())) < 40, database, table FROM system.kafka_consumers WHERE table in('kafka_Avro', 'kafka_JSONEachRow') ORDER BY table, assignments.partition_id[1]
        """,
        retry_count=max_retries,
        sleep_time=1,
        check_callback=lambda res: res.replace("\t", "|") == expected_result,
    )

    assert result_system_kafka_consumers.replace("\t", "|") == expected_result

    for format_name in [
        "Avro",
        "JSONEachRow",
    ]:
        kafka_delete_topic(admin_client, f"{format_name}_parsing_err")


def test_bad_messages_to_mv(kafka_cluster, max_retries=20):
    admin_client = KafkaAdminClient(
        bootstrap_servers="localhost:{}".format(kafka_cluster.kafka_port)
    )

    kafka_create_topic(admin_client, "tomv")

    instance.query(
        f"""
        DROP TABLE IF EXISTS kafka_materialized;
        DROP TABLE IF EXISTS kafka_consumer;
        DROP TABLE IF EXISTS kafka1;

        CREATE TABLE kafka1 (key UInt64, value String)
            ENGINE = Kafka
            SETTINGS kafka_broker_list = 'kafka1:19092',
                     kafka_topic_list = 'tomv',
                     kafka_group_name = 'tomv',
                     kafka_format = 'JSONEachRow',
                     kafka_flush_interval_ms=1000,
                     kafka_num_consumers = 1;

        CREATE TABLE kafka_materialized(`key` UInt64, `value` UInt64) ENGINE = Log;

        CREATE MATERIALIZED VIEW kafka_consumer TO kafka_materialized
        (`key` UInt64, `value` UInt64) AS
        SELECT key, CAST(value, 'UInt64') AS value
        FROM kafka1;
    """
    )

    kafka_produce(kafka_cluster, "tomv", ['{"key":10, "value":"aaa"}'])

    expected_result = """Code: 6. DB::Exception: Cannot parse string \\'aaa\\' as UInt64: syntax error at begin of string. Note: there are toUInt64OrZero and to|1|1|1|default|kafka1
"""
    result_system_kafka_consumers = instance.query_with_retry(
        """
        SELECT substr(exceptions.text[1], 1, 131), length(exceptions.text) > 1 AND length(exceptions.text) < 15, length(exceptions.time) > 1 AND length(exceptions.time) < 15, abs(dateDiff('second', exceptions.time[1], now())) < 40, database, table FROM system.kafka_consumers  WHERE table='kafka1' ORDER BY table, assignments.partition_id[1]
        """,
        retry_count=max_retries,
        sleep_time=1,
        check_callback=lambda res: res.replace("\t", "|") == expected_result,
    )

    assert result_system_kafka_consumers.replace("\t", "|") == expected_result

    kafka_delete_topic(admin_client, "tomv")


def test_system_kafka_consumers_grant(kafka_cluster, max_retries=20):
    admin_client = KafkaAdminClient(
        bootstrap_servers="localhost:{}".format(kafka_cluster.kafka_port)
    )

    kafka_create_topic(admin_client, "visible")
    kafka_create_topic(admin_client, "hidden")
    instance.query(
        f"""
        DROP TABLE IF EXISTS kafka_grant_visible;
        DROP TABLE IF EXISTS kafka_grant_hidden;

        CREATE TABLE kafka_grant_visible (key UInt64, value String)
            ENGINE = Kafka
            SETTINGS kafka_broker_list = 'kafka1:19092',
                     kafka_topic_list = 'visible',
                     kafka_group_name = 'visible',
                     kafka_format = 'JSONEachRow',
                     kafka_flush_interval_ms=1000,
                     kafka_num_consumers = 1;

        CREATE TABLE kafka_grant_hidden (key UInt64, value String)
            ENGINE = Kafka
            SETTINGS kafka_broker_list = 'kafka1:19092',
                     kafka_topic_list = 'hidden',
                     kafka_group_name = 'hidden',
                     kafka_format = 'JSONEachRow',
                     kafka_flush_interval_ms=1000,
                     kafka_num_consumers = 1;
    """
    )

    result_system_kafka_consumers = instance.query_with_retry(
        """
        SELECT count(1) FROM system.kafka_consumers WHERE table LIKE 'kafka_grant%'
        """,
        retry_count=max_retries,
        sleep_time=1,
        check_callback=lambda res: int(res) == 2,
    )
    # both kafka_grant_hidden and kafka_grant_visible tables are visible

    instance.query(
        f"""
        CREATE USER RESTRICTED;
        GRANT SHOW ON default.kafka_grant_visible TO RESTRICTED;
        GRANT SELECT ON system.kafka_consumers TO RESTRICTED;
    """
    )

    restricted_result_system_kafka_consumers = instance.query(
        "SELECT count(1) FROM system.kafka_consumers WHERE table LIKE 'kafka_grant%'",
        user="RESTRICTED",
    )
    assert int(restricted_result_system_kafka_consumers) == 1
    # only kafka_grant_visible is visible for user RESTRICTED

    kafka_delete_topic(admin_client, "visible")
    kafka_delete_topic(admin_client, "hidden")
    instance.query(
        f"""
        DROP TABLE IF EXISTS kafka_grant_visible;
        DROP TABLE IF EXISTS kafka_grant_hidden;
        DROP USER RESTRICTED;
    """
    )


def test_log_to_exceptions(kafka_cluster, max_retries=20):

    non_existent_broker_port = 9876
    instance.query(
        f"""
        DROP TABLE IF EXISTS foo_exceptions;

        CREATE TABLE foo_exceptions(a String)
            ENGINE = Kafka
            SETTINGS kafka_broker_list = 'localhost:{non_existent_broker_port}', kafka_topic_list = 'foo', kafka_group_name = 'foo', kafka_format = 'RawBLOB';
    """
    )

    instance.query("SELECT * FROM foo_exceptions SETTINGS stream_like_engine_allow_direct_select=1")
    instance.query("SYSTEM FLUSH LOGS")

    system_kafka_consumers_content = instance.query("SELECT exceptions.text FROM system.kafka_consumers ARRAY JOIN exceptions WHERE table LIKE 'foo_exceptions' LIMIT 1")

    logging.debug(
        f"system.kafka_consumers content: {system_kafka_consumers_content}"
    )
    assert system_kafka_consumers_content.startswith(f"[thrd:localhost:{non_existent_broker_port}/bootstrap]: localhost:{non_existent_broker_port}/bootstrap: Connect to ipv4#127.0.0.1:{non_existent_broker_port} failed: Connection refused")

    instance.query("DROP TABLE foo_exceptions")


if __name__ == "__main__":
    cluster.start()
    input("Cluster created, press any key to destroy...")
    cluster.shutdown()
