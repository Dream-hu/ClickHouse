import copy
import dataclasses
import datetime
import json
import random
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Union

from ._environment import _Environment
from .s3 import S3, StorageUsage
from .settings import Settings
from .utils import ContextManager, MetaClasses, Shell, Utils


@dataclasses.dataclass
class Result(MetaClasses.Serializable):
    """
    Represents the outcome of a workflow/job/task or any operation, along with associated metadata.

    This class supports nesting of results to represent tasks with sub-tasks, and includes
    various attributes to track status, timing, files, and links.

    Attributes:
        name (str): The name of the task.
        status (str): The current status of the task. Should be one of the values defined in the Status class.
        start_time (Optional[float]): The start time of the task in Unix timestamp format. None if not started.
        duration (Optional[float]): The duration of the task in seconds. None if not completed.
        results (List[Result]): A list of sub-results representing nested tasks.
        files (List[str]): A list of file paths or names related to the result.
        links (List[str]): A list of URLs related to the result (e.g., links to reports or resources).
        info (str): Additional information about the result. Free-form text.

    Inner Class:
        Status: Defines possible statuses for the task, such as "success", "failure", etc.
    """

    class Status:
        SKIPPED = "skipped"
        SUCCESS = "success"
        FAILED = "failure"
        PENDING = "pending"
        RUNNING = "running"
        ERROR = "error"

    name: str
    status: str
    start_time: Optional[float] = None
    duration: Optional[float] = None
    results: List["Result"] = dataclasses.field(default_factory=list)
    files: List[str] = dataclasses.field(default_factory=list)
    links: List[str] = dataclasses.field(default_factory=list)
    info: str = ""
    ext: Dict[str, Any] = dataclasses.field(default_factory=dict)

    @staticmethod
    def create_from(
        name="",
        results: List["Result"] = None,
        stopwatch: Utils.Stopwatch = None,
        status="",
        files=None,
        info: Union[List[str], str] = "",
        with_info_from_results=False,
    ) -> "Result":
        if isinstance(status, bool):
            status = Result.Status.SUCCESS if status else Result.Status.FAILED
        if not results and not status:
            status = Result.Status.ERROR
        if not name:
            name = _Environment.get().JOB_NAME
            if not name:
                print("ERROR: Failed to guess the .name")
                raise
        start_time = None
        duration = None
        if not stopwatch:
            try:
                preresult = Result.from_fs(name=name)
                start_time = preresult.start_time
                duration = datetime.datetime.now().timestamp() - preresult.start_time
            except Exception:
                print(
                    f"WARNING: Failed to get start time for [{name}] - start time and duration won't be set"
                )
        else:
            start_time = stopwatch.start_time
            duration = stopwatch.duration

        result_status = status or Result.Status.SUCCESS
        infos = []
        if info:
            if isinstance(info, str):
                infos += [info]
            else:
                infos += info
        if results and not status:
            for result in results:
                if result.status in (Result.Status.SUCCESS, Result.Status.SKIPPED):
                    continue
                elif result.status == Result.Status.ERROR:
                    result_status = Result.Status.ERROR
                    break
                elif result.status == Result.Status.FAILED:
                    result_status = Result.Status.FAILED
                else:
                    Utils.raise_with_error(
                        f"Unexpected result status [{result.status}] for [{result.name}]"
                    )
        if results and with_info_from_results:
            for result in results:
                if result.info:
                    infos.append(f"{result.name}: {result.info}")
        return Result(
            name=name,
            status=result_status,
            start_time=start_time,
            duration=duration,
            info="\n".join(infos) if infos else "",
            results=results or [],
            files=files or [],
        )

    @staticmethod
    def get():
        return Result.from_fs(_Environment.get().JOB_NAME)

    @staticmethod
    def get_workflow_result():
        """
        Returns the latest workflow result, if available on fs
        :return:
        """
        return Result.from_fs(_Environment.get().WORKFLOW_NAME)

    def is_completed(self):
        return self.status not in (Result.Status.PENDING, Result.Status.RUNNING)

    def is_running(self):
        return self.status in (Result.Status.RUNNING,)

    def is_ok(self):
        return self.status in (Result.Status.SKIPPED, Result.Status.SUCCESS)

    def is_error(self):
        return self.status in (Result.Status.ERROR,)

    def set_status(self, status) -> "Result":
        self.status = status
        self.dump()
        return self

    def set_success(self) -> "Result":
        return self.set_status(Result.Status.SUCCESS)

    def set_failed(self) -> "Result":
        return self.set_status(Result.Status.FAILED)

    def set_results(self, results: List["Result"]) -> "Result":
        self.results = results
        self.dump()
        return self

    def set_files(self, files) -> "Result":
        if isinstance(files, (str, Path)):
            files = [files]
        for file in files:
            assert Path(
                file
            ).is_file(), f"Not valid file [{file}] from file list [{files}]"
        if not self.files:
            self.files = []
        for file in self.files:
            if file in files:
                print(
                    f"WARNING: File [{file}] is already present in Result [{self.name}] - skip"
                )
                files.remove(file)
        self.files += files
        self.dump()
        return self

    def set_info(self, info: str) -> "Result":
        if self.info:
            self.info += "\n"
        self.info += info
        self.dump()
        return self

    def set_link(self, link) -> "Result":
        self.links.append(link)
        self.dump()
        return self

    def _add_job_summary_to_info(self):
        subresult_with_tests = self
        with_test_in_run_command = False

        # Use a specific sub-result if configured
        job_config = _Environment.get().JOB_CONFIG or {}
        result_name_for_cidb = job_config.get("result_name_for_cidb", "")
        if result_name_for_cidb:
            for r in self.results:
                if r.name == result_name_for_cidb:
                    subresult_with_tests = r
                    with_test_in_run_command = True
                    if subresult_with_tests.info:
                        self.set_info(subresult_with_tests.info)
                    break

        # If no failures, nothing more to do
        if self.is_ok():
            return self

        # Collect failed test case names
        failed = [r.name for r in subresult_with_tests.results if not r.is_ok()]

        if failed:
            if len(failed) < 10:
                failed_tcs = ", ".join(failed)
                self.set_info(f"Failed: {failed_tcs}")

        # Suggest local command to rerun
        command_info = f'To run locally: python -m ci.praktika run "{self.name}"'
        if with_test_in_run_command and failed:
            command_info += f" --test {failed[0]}"
        self.set_info(command_info)

        return self

    @classmethod
    def file_name_static(cls, name):
        if not name:
            return cls.experimental_file_name_static()
        else:
            return f"{Settings.TEMP_DIR}/result_{Utils.normalize_string(name)}.json"

    @classmethod
    def experimental_file_name_static(cls):
        return f"{Settings.TEMP_DIR}/result.json"

    @classmethod
    def experimental_from_fs(cls, name):
        # experimental mode to let job write results into fixed result.json file instead of result_job_name.json
        Shell.check(
            f"cp {cls.experimental_file_name_static()} {cls.file_name_static(name)}",
            verbose=True,
        )
        result = Result.from_fs(name)
        result.name = name
        result.dump()
        return result

    @classmethod
    def from_dict(cls, obj: Dict[str, Any]) -> "Result":
        sub_results = []
        for result_dict in obj.get("results", []):
            sub_res = cls.from_dict(result_dict)
            sub_results.append(sub_res)
        obj["results"] = sub_results
        return Result(**obj)

    def update_duration(self):
        if self.duration:
            return self
        if self.start_time:
            self.duration = datetime.datetime.now().timestamp() - self.start_time
        else:
            print(
                f"NOTE: start_time is not set for job [{self.name}] Result - do not update duration"
            )
        return self

    def set_timing(self, stopwatch: Utils.Stopwatch):
        self.start_time = stopwatch.start_time
        self.duration = stopwatch.duration
        return self

    def update_sub_result(self, result: "Result", drop_nested_results=False):
        assert self.results, "BUG?"
        for i, result_ in enumerate(self.results):
            if result_.name == result.name:
                if drop_nested_results:
                    res_ = copy.deepcopy(result)
                    res_.results = []
                    self.results[i] = res_
                else:
                    self.results[i] = result
        self._update_status()
        return self

    def _update_status(self):
        was_pending = False
        was_running = False
        if self.status == self.Status.PENDING:
            was_pending = True
        if self.status == self.Status.RUNNING:
            was_running = True

        has_pending, has_running, has_failed = False, False, False
        for result_ in self.results:
            if result_.status in (self.Status.RUNNING,):
                has_running = True
            if result_.status in (self.Status.PENDING,):
                has_pending = True
            if result_.status in (self.Status.ERROR, self.Status.FAILED):
                has_failed = True
        if has_running:
            self.status = self.Status.RUNNING
        elif has_pending:
            self.status = self.Status.PENDING
        elif has_failed:
            self.status = self.Status.FAILED
        else:
            self.status = self.Status.SUCCESS
        if (was_pending or was_running) and self.status not in (
            self.Status.PENDING,
            self.Status.RUNNING,
        ):
            print("Pipeline finished")
            self.update_duration()

    def add_ext_key_value(self, key, value):
        self.ext[key] = value
        return self

    @classmethod
    def generate_pending(cls, name, results=None):
        return Result(
            name=name,
            status=Result.Status.PENDING,
            start_time=None,
            duration=None,
            results=results or [],
            files=[],
            links=[],
            info="",
        )

    @classmethod
    def generate_skipped(cls, name, links=None, info="", results=None):
        return Result(
            name=name,
            status=Result.Status.SKIPPED,
            start_time=None,
            duration=None,
            results=results or [],
            files=[],
            links=links or [],
            info=info,
        )

    @classmethod
    def from_gtest_run(cls, unit_tests_path, name="", with_log=False):
        """
        Runs gtest and generates praktika Result
        :param unit_tests_path:
        :param name: Should be set if executed as a job subtask with name @name.
        If it's a job itself job.name will be taken as name by default
        :param with_log:
        :return:
        """
        Shell.check(f"rm {ResultTranslator.GTEST_RESULT_FILE}")
        result = Result.from_commands_run(
            name=name,
            command=[
                f"chmod +x {unit_tests_path}",
                f"{unit_tests_path} --gtest_output='json:{ResultTranslator.GTEST_RESULT_FILE}'",
            ],
            with_log=with_log,
        )
        status, results, info = ResultTranslator.from_gtest()
        result.set_status(status).set_results(results).set_info(info)
        return result

    @classmethod
    def from_commands_run(
        cls,
        name,
        command,
        with_log=False,
        with_info=False,
        fail_fast=True,
        workdir=None,
        command_args=None,
        command_kwargs=None,
    ):
        """
        Executes shell commands or Python callables, optionally logging output, and handles errors.

        :param name: Check name
        :param command: Shell command (str) or Python callable, or list of them.
        :param workdir: Optional working directory.
        :param with_log: Boolean flag to log output to a file.
        :param with_info: Fill in Result.info from command output
        :param fail_fast: Boolean flag to stop execution if one command fails.
        :param command_args: Positional arguments for the callable command.
        :param command_kwargs: Keyword arguments for the callable command.
        :return: Result object with status and optional log file.
        """

        # Stopwatch to track execution time
        stop_watch_ = Utils.Stopwatch()
        command_args = command_args or []
        command_kwargs = command_kwargs or {}

        # Set log file path if logging is enabled
        if with_log:
            log_file = f"{Utils.absolute_path(Settings.TEMP_DIR)}/{Utils.normalize_string(name)}.log"
        elif with_info:
            log_file = f"/tmp/praktika_{Utils.normalize_string(name)}.log"
        else:
            log_file = None

        # Ensure the command is a list for consistent iteration
        if not isinstance(command, list):
            fail_fast = False
            command = [command]

        print(f"> Starting execution for [{name}]")
        res = True  # Track success/failure status
        error_infos = []
        with ContextManager.cd(workdir):
            for command_ in command:
                if callable(command_):
                    # If command is a Python function, call it with provided arguments
                    result = command_(*command_args, **command_kwargs)
                    if isinstance(result, bool):
                        res = result
                    elif result:
                        error_infos.append(str(result))
                        res = False
                else:
                    # Run shell command in a specified directory with logging and verbosity
                    exit_code = Shell.run(command_, verbose=True, log_file=log_file)
                    if with_info:
                        with open(log_file, "r") as f:
                            error_infos.append(f.read().strip())
                    res = exit_code == 0

                # If fail_fast is enabled, stop on first failure
                if not res and fail_fast:
                    print(f"Execution stopped due to failure in [{command_}]")
                    break

        # Create and return the result object with status and log file (if any)
        return Result.create_from(
            name=name,
            status=res,
            stopwatch=stop_watch_,
            info=error_infos,
            files=[log_file] if with_log else None,
        )

    def complete_job(self, with_job_summary_in_info=True):
        if with_job_summary_in_info:
            self._add_job_summary_to_info()
        self.dump()
        if not self.is_ok():
            print("ERROR: Job Failed")
            print(self.to_stdout_formatted())
            sys.exit(1)
        else:
            print("ok")

    def to_stdout_formatted(self, indent="", res=""):
        if self.is_ok():
            return res

        res += f"{indent}Task [{self.name}] failed.\n"
        fail_info = ""
        sub_indent = indent + "  "

        if not self.results:
            if not self.is_ok():
                fail_info += f"{sub_indent}{self.name}:\n"
                for line in self.info.splitlines():
                    fail_info += f"{sub_indent}{sub_indent}{line}\n"
            return res + fail_info

        for sub_result in self.results:
            res = sub_result.to_stdout_formatted(sub_indent, res)

        return res


class ResultInfo:
    SETUP_ENV_JOB_FAILED = (
        "Failed to set up job env, it's praktika bug or misconfiguration"
    )
    PRE_JOB_FAILED = (
        "Failed to do a job pre-run step, it's praktika bug or misconfiguration"
    )
    KILLED = "Job killed or terminated, no Result provided"
    NOT_FOUND_IMPOSSIBLE = (
        "No Result file (bug, or job misbehaviour, must not ever happen)"
    )
    SKIPPED_DUE_TO_PREVIOUS_FAILURE = "Skipped due to previous failure"
    TIMEOUT = "Timeout"

    GH_STATUS_ERROR = "Failed to set GH commit status"

    NOT_FINALIZED = (
        "Job did not provide Result: job script bug, died CI runner or praktika bug"
    )

    S3_ERROR = "S3 call failure"


class _ResultS3:

    @classmethod
    def copy_result_to_s3(cls, result, clean=False):
        result.dump()
        env = _Environment.get()
        s3_path = f"{Settings.HTML_S3_PATH}/{env.get_s3_prefix()}"
        if clean:
            S3.delete(s3_path)
        url = S3.copy_file_to_s3(s3_path=s3_path, local_path=result.file_name())
        return url

    @classmethod
    def copy_result_from_s3(cls, local_path):
        env = _Environment.get()
        file_name = Path(local_path).name
        s3_path = f"{Settings.HTML_S3_PATH}/{env.get_s3_prefix()}/{file_name}"
        S3.copy_file_from_s3(s3_path=s3_path, local_path=local_path)

    @classmethod
    def copy_result_from_s3_with_version(cls, local_path):
        env = _Environment.get()
        file_name = Path(local_path).name
        local_dir = Path(local_path).parent
        file_name_pattern = f"{file_name}_*"
        for file_path in local_dir.glob(file_name_pattern):
            file_path.unlink()
        s3_path = f"{Settings.HTML_S3_PATH}/{env.get_s3_prefix()}/"
        S3.copy_file_from_s3_matching_pattern(
            s3_path=s3_path, local_path=local_dir, include=file_name_pattern
        )
        result_files = []
        for file_path in local_dir.glob(file_name_pattern):
            result_files.append(file_path)
        assert result_files, "No result files found"
        result_files.sort()
        version = int(result_files[-1].name.split("_")[-1])
        Shell.check(f"cp {result_files[-1]} {local_path}", strict=True, verbose=True)
        return version

    @classmethod
    def copy_result_to_s3_with_version(cls, result, version, no_strict=False):
        result.dump()
        filename = Path(result.file_name()).name
        file_name_versioned = f"{filename}_{str(version).zfill(3)}"
        env = _Environment.get()
        s3_path = f"{Settings.HTML_S3_PATH}/{env.get_s3_prefix()}/"
        s3_path_versioned = f"{s3_path}{file_name_versioned}"
        if version == 0:
            S3.clean_s3_directory(s3_path=s3_path, include=f"{filename}*")
        if not S3.put(
            s3_path=s3_path_versioned,
            local_path=result.file_name(),
            if_none_matched=True,
            no_strict=no_strict,
        ):
            print("Failed to put versioned Result")
            return False
        if not S3.put(
            s3_path=s3_path, local_path=result.file_name(), no_strict=no_strict
        ):
            print("Failed to put non-versioned Result")
        return True

    @classmethod
    def upload_result_files_to_s3(
        cls, result: Result, s3_subprefix="", _uploaded_file_link=None
    ):
        s3_subprefix = "/".join([s3_subprefix, Utils.normalize_string(result.name)])

        if not _uploaded_file_link:
            _uploaded_file_link = {}

        for file in result.files:
            if not Path(file).is_file():
                print(f"ERROR: Invalid file [{file}] in [{result.name}] - skip upload")
                result.set_info(f"WARNING: File [{file}] was not found")
                file_link = S3._upload_file_to_s3(file, upload_to_s3=False)
            elif file in _uploaded_file_link:
                # in case different sub results have the same file for upload
                file_link = _uploaded_file_link[file]
            else:
                is_text = False
                for text_file_suffix in Settings.TEXT_CONTENT_EXTENSIONS:
                    if file.endswith(text_file_suffix):
                        print(
                            f"File [{file}] matches Settings.TEXT_CONTENT_EXTENSIONS [{Settings.TEXT_CONTENT_EXTENSIONS}] - add text attribute for s3 object"
                        )
                        is_text = True
                        break
                file_link = S3._upload_file_to_s3(
                    file,
                    upload_to_s3=True,
                    text=is_text,
                    s3_subprefix=s3_subprefix,
                )
                _uploaded_file_link[file] = file_link
            result.links.append(file_link)
        result.files = []

        if result.results:
            for result_ in result.results:
                cls.upload_result_files_to_s3(
                    result_,
                    s3_subprefix=s3_subprefix,
                    _uploaded_file_link=_uploaded_file_link,
                )
        return result

    @classmethod
    def update_workflow_results(
        cls, workflow_name, new_info="", new_sub_results=None, storage_usage=None
    ):
        assert new_info or new_sub_results

        attempt = 1
        prev_status = ""
        new_status = ""
        MAX_ATTEMPTS = 50

        while attempt < MAX_ATTEMPTS:
            version = cls.copy_result_from_s3_with_version(
                Result.file_name_static(workflow_name)
            )
            workflow_result = Result.from_fs(workflow_name)
            prev_status = workflow_result.status
            if new_info:
                workflow_result.set_info(new_info)
            if new_sub_results:
                if isinstance(new_sub_results, Result):
                    new_sub_results = [new_sub_results]
                for result_ in new_sub_results:
                    workflow_result.update_sub_result(
                        result_, drop_nested_results=True
                    ).dump()
            if storage_usage:
                workflow_storage_usage = StorageUsage.from_dict(
                    workflow_result.ext.get("storage_usage", {})
                ).merge_with(storage_usage)
                workflow_result.ext["storage_usage"] = workflow_storage_usage

            new_status = workflow_result.status
            if cls.copy_result_to_s3_with_version(
                workflow_result,
                version=version + 1,
                no_strict=attempt < MAX_ATTEMPTS - 1,
            ):
                break
            print(f"Attempt [{attempt}] to upload workflow result failed")
            attempt += 1
            # random delay (0-2s) to reduce contention and minimize race conditions
            # when multiple concurrent jobs attempt to update the workflow report
            time.sleep(random.uniform(0, 2))

        if prev_status != new_status:
            return new_status
        else:
            return None


class ResultTranslator:
    GTEST_RESULT_FILE = "./ci/tmp/gtest.json"

    @classmethod
    def from_gtest(cls):
        """The json is described by the next proto3 scheme:
        (It's wrong, but that's a copy/paste from
        https://google.github.io/googletest/advanced.html#generating-a-json-report)

        syntax = "proto3";

        package googletest;

        import "google/protobuf/timestamp.proto";
        import "google/protobuf/duration.proto";

        message UnitTest {
          int32 tests = 1;
          int32 failures = 2;
          int32 disabled = 3;
          int32 errors = 4;
          google.protobuf.Timestamp timestamp = 5;
          google.protobuf.Duration time = 6;
          string name = 7;
          repeated TestCase testsuites = 8;
        }

        message TestCase {
          string name = 1;
          int32 tests = 2;
          int32 failures = 3;
          int32 disabled = 4;
          int32 errors = 5;
          google.protobuf.Duration time = 6;
          repeated TestInfo testsuite = 7;
        }

        message TestInfo {
          string name = 1;
          string file = 6;
          int32 line = 7;
          enum Status {
            RUN = 0;
            NOTRUN = 1;
          }
          Status status = 2;
          google.protobuf.Duration time = 3;
          string classname = 4;
          message Failure {
            string failures = 1;
            string type = 2;
          }
          repeated Failure failures = 5;
        }"""

        test_results = []  # type: List[Result]

        if not Path(cls.GTEST_RESULT_FILE).exists():
            print(f"ERROR: No test result file [{cls.GTEST_RESULT_FILE}]")
            return (
                Result.Status.ERROR,
                test_results,
                f"No test result file [{cls.GTEST_RESULT_FILE}]",
            )

        try:
            with open(
                cls.GTEST_RESULT_FILE, "r", encoding="utf-8", errors="ignore"
            ) as j:
                report = json.load(j)
        except Exception as e:
            print(f"ERROR: failed to read json [{e}]")
            return (
                Result.Status.ERROR,
                [
                    Result(
                        name="Parsing Error",
                        status=Result.Status.ERROR,
                        files=[cls.GTEST_RESULT_FILE],
                        info=str(e),
                    )
                ],
                "ERROR: failed to read gtest json",
            )

        total_counter = report["tests"]
        failed_counter = report["failures"]
        error_counter = report["errors"]

        description = ""
        SEGFAULT = "Segmentation fault. "
        SIGNAL = "Exit on signal. "
        for suite in report["testsuites"]:
            suite_name = suite["name"]
            for test_case in suite["testsuite"]:
                case_name = test_case["name"]
                test_time = float(test_case["time"][:-1])
                raw_logs = None
                if "failures" in test_case:
                    raw_logs = ""
                    for failure in test_case["failures"]:
                        raw_logs += failure[Result.Status.FAILED]
                    if (
                        "Segmentation fault" in raw_logs  # type: ignore
                        and SEGFAULT not in description
                    ):
                        description += SEGFAULT
                    if (
                        "received signal SIG" in raw_logs  # type: ignore
                        and SIGNAL not in description
                    ):
                        description += SIGNAL
                if test_case["status"] == "NOTRUN":
                    test_status = "SKIPPED"
                elif raw_logs is None:
                    test_status = Result.Status.SUCCESS
                else:
                    test_status = Result.Status.FAILED

                test_results.append(
                    Result(
                        f"{suite_name}.{case_name}",
                        test_status,
                        duration=test_time,
                        info=raw_logs,
                    )
                )

        check_status = Result.Status.SUCCESS
        test_status = Result.Status.SUCCESS
        tests_time = float(report["time"][:-1])
        if failed_counter:
            check_status = Result.Status.FAILED
            test_status = Result.Status.FAILED
        if error_counter:
            check_status = Result.Status.ERROR
            test_status = Result.Status.ERROR
        test_results.append(Result(report["name"], test_status, duration=tests_time))

        if not description:
            description += (
                f"fail: {failed_counter + error_counter}, "
                f"passed: {total_counter - failed_counter - error_counter}"
            )

        return (
            check_status,
            test_results,
            description,
        )
