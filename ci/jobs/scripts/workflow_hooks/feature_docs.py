import sys

from praktika.info import Info

from ci.jobs.scripts.workflow_hooks.pr_description import Labels

files_for_which_docs_autogenerated = ["src/storages/MergeTree/MergeTreeSettings.cpp", "src/Core/Settings.cpp"]

def check_docs():
    info = Info()
    if Labels.PR_FEATURE in info.pr_labels:
        changed_files = info.get_custom_data("changed_files")
        
        if not any(file.startswith("docs/") or file in files_for_which_docs_autogenerated for file in changed_files):
            print("No changes in docs for new feature")
            return False
    return True


if __name__ == "__main__":
    if not check_docs():
        sys.exit(1)
