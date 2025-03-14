from praktika import Workflow

from ci.defs.defs import SECRETS, ArtifactConfigs
from ci.defs.job_configs import JobConfigs
from ci.jobs.scripts.workflow_hooks.filter_job import should_skip_job

workflow = Workflow.Config(
    name="ReleaseBranchCI",
    event=Workflow.Event.PUSH,
    branches=["2[1-9].[1-9][0-9]", "2[1-9].[1-9]"],
    jobs=[
        *JobConfigs.build_jobs,
        *[
            job
            for job in JobConfigs.special_build_jobs
            if any(t in job.name for t in ("darwin",))
        ],
        JobConfigs.docker_sever,
        JobConfigs.docker_keeper,
        *JobConfigs.install_check_jobs,
        *[
            job
            for job in JobConfigs.integration_test_asan_master_jobs
            if "asan" in job.name
        ],
        *[
            job
            for job in JobConfigs.integration_test_jobs_required
            if any(t in job.name for t in ("asan", "release"))
        ],
        *[
            job
            for job in JobConfigs.integration_test_jobs_non_required
            if "tsan" in job.name
        ],
        *JobConfigs.stress_test_jobs,
    ],
    artifacts=[
        *ArtifactConfigs.unittests_binaries,
        *ArtifactConfigs.clickhouse_binaries,
        *ArtifactConfigs.clickhouse_debians,
        *ArtifactConfigs.clickhouse_rpms,
        *ArtifactConfigs.clickhouse_tgzs,
        *ArtifactConfigs.performance_packages,
    ],
    # dockers=DOCKERS,
    secrets=SECRETS,
    enable_cache=True,
    enable_report=True,
    enable_cidb=True,
    enable_commit_status_on_failure=True,
    pre_hooks=[
        "python3 ./ci/jobs/scripts/workflow_hooks/store_data.py",
        "python3 ./ci/jobs/scripts/workflow_hooks/version_log.py",
    ],
    workflow_filter_hooks=[should_skip_job],
    post_hooks=[],
)

WORKFLOWS = [
    workflow,
]
