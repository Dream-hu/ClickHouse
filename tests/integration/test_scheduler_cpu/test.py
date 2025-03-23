# pylint: disable=unused-argument
# pylint: disable=redefined-outer-name
# pylint: disable=line-too-long

import random
import threading
import time

import pytest

from helpers.client import QueryRuntimeException
from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

node = cluster.add_instance(
    "node",
    stay_alive=True,
    main_configs=[],
    with_zookeeper=True,
)


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    try:
        cluster.start()
        yield
    finally:
        cluster.shutdown()


@pytest.fixture(scope="function", autouse=True)
def clear_workloads_and_resources():
    node.query(
        f"""
        drop workload if exists production;
        drop workload if exists development;
        drop workload if exists admin;
        drop workload if exists all;
        drop resource if exists cpu;
    """
    )
    yield


def assert_profile_event(node, query_id, profile_event, check):
    assert check(
        int(
            node.query(
                f"select ProfileEvents['{profile_event}'] from system.query_log where current_database = currentDatabase() and query_id = '{query_id}' and type = 'QueryFinish' order by query_start_time_microseconds desc limit 1"
            )
        )
    )


def test_create_workload():
    node.query(
        f"""
        create resource cpu (master thread, worker thread);
        create workload all settings max_concurrent_threads = 100;
        create workload admin in all settings priority = 0;
        create workload production in all settings priority = 1, weight = 9;
        create workload development in all settings priority = 1, weight = 1;
    """
    )

    def do_checks():
        assert (
            node.query(
                f"select count() from system.scheduler where path ilike '%/admin/%' and type='fifo'"
            )
            == "1\n"
        )
        assert (
            node.query(
                f"select count() from system.scheduler where path ilike '%/admin' and type='unified' and priority=0"
            )
            == "1\n"
        )
        assert (
            node.query(
                f"select count() from system.scheduler where path ilike '%/production/%' and type='fifo'"
            )
            == "1\n"
        )
        assert (
            node.query(
                f"select count() from system.scheduler where path ilike '%/production' and type='unified' and weight=9"
            )
            == "1\n"
        )
        assert (
            node.query(
                f"select count() from system.scheduler where path ilike '%/development/%' and type='fifo'"
            )
            == "1\n"
        )
        assert (
            node.query(
                f"select count() from system.scheduler where path ilike '%/all/%' and type='inflight_limit' and resource='cpu' and max_requests=100"
            )
            == "1\n"
        )

    do_checks()
    node.restart_clickhouse()  # Check that workloads persist
    do_checks()


def test_concurrency_control_compatibility():
    node.query(
        f"""
        create resource cpu (worker thread); -- concurrency control doesn't count master thread towards the cpu slots limit
        create workload all settings max_concurrent_threads = 50;
    """
    )

    node.query(
        "select count(*) from numbers_mt(10000000) settings max_threads=100, workload='all'",
        query_id="test_compatibility",
    )
    node.query("SYSTEM FLUSH LOGS")
    assert_profile_event(
        node,
        "test_compatibility",
        "ConcurrencyControlSlotsAcquired",
        lambda x: x >= 1 and x <= 50,
    )
    assert_profile_event(
        node,
        "test_compatibility",
        "ConcurrencyControlSlotsAcquiredNonCompeting",
        lambda x: x == 1,
    )
    s_count = node.query(
        "select length(thread_ids) from system.query_log where current_database = currentDatabase() and type = 'QueryFinish' and query_id = 'test_compatibility' order by query_start_time_microseconds desc limit 1"
    ).strip()
    count = int(s_count) if s_count else 0
    assert count >= 3 and count <= 53

    node.query(
        f"""
        create or replace workload all settings max_concurrent_threads = 1;
    """
    )

    node.query(
        "select count(*) from numbers_mt(10000000) settings max_threads=100, workload='all'",
        query_id="test_compatibility",
    )
    node.query("SYSTEM FLUSH LOGS")

    assert (
        int(
            node.query(
                f"select inflight_requests from system.scheduler where path ilike '%/all/%' and type='inflight_limit' and resource='cpu'"
            )
        ) == 0
    )

    assert_profile_event(
        node,
        "test_compatibility",
        "ConcurrencyControlSlotsAcquired",
        lambda x: x == 1,
    )
    assert_profile_event(
        node,
        "test_compatibility",
        "ConcurrencyControlSlotsAcquiredNonCompeting",
        lambda x: x == 1,
    )
    assert (
        node.query(
            "select length(thread_ids) from system.query_log where current_database = currentDatabase() and type = 'QueryFinish' and query_id = 'test_compatibility' order by query_start_time_microseconds desc limit 1"
        )
        == "4\n"
    )
