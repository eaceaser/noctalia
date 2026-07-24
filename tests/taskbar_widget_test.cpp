#include "shell/bar/widgets/taskbar_widget.h"

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class TaskbarWidgetTestAccess {
public:
  static std::pair<bool, bool> compare(
      bool showWindowTitle, std::uintptr_t previousHandle, std::uintptr_t nextHandle, std::string previousTitle,
      std::string nextTitle
  ) {
    const TaskbarWidget::TaskModel previous{
        .handleKey = previousHandle,
        .title = std::move(previousTitle),
    };
    const TaskbarWidget::TaskModel next{
        .handleKey = nextHandle,
        .title = std::move(nextTitle),
    };
    const auto comparison = TaskbarWidget::compareModels(showWindowTitle, {previous}, {}, {next}, {});
    return {comparison.layoutEqual, comparison.titlesChanged};
  }

  // Resolves a retained tile's task reference the way the live tile callbacks do.
  static std::optional<std::string> resolvedTitle(
      const std::vector<TaskbarWidget::TaskModel>& tasks, std::size_t index, std::uint64_t referenceGeneration,
      std::uint64_t currentGeneration
  ) {
    const auto* current =
        TaskbarWidget::resolveTask(tasks, {.index = index, .generation = referenceGeneration}, currentGeneration);
    return current != nullptr ? std::optional<std::string>(current->title) : std::nullopt;
  }

  static TaskbarWidget::TaskModel task(std::uintptr_t handleKey, std::string title) {
    return {
        .handleKey = handleKey,
        .title = std::move(title),
    };
  }

  static std::pair<std::string, std::string>
  rebindWorkspaceWindow(std::string exactWindowId, std::string assignedWindowId) {
    TaskbarWidget::TaskModel task{
        .workspaceWindowId = exactWindowId,
        .exactCompositorWindowId = std::move(exactWindowId),
    };
    task.workspaceWindowId = std::move(assignedWindowId);
    return {task.workspaceWindowId, task.exactCompositorWindowId};
  }

  static std::string bindingWindowId(std::string workspaceWindowId, std::string exactWindowId) {
    return std::string(
        TaskbarWidget::workspaceBindingWindowId(
            TaskbarWidget::TaskModel{
                .workspaceWindowId = std::move(workspaceWindowId),
                .exactCompositorWindowId = std::move(exactWindowId),
            }
        )
    );
  }

  static bool exactWindowIdChangeKeepsLayout(std::string previousId, std::string nextId) {
    const TaskbarWidget::TaskModel previous{
        .handleKey = 11,
        .exactCompositorWindowId = std::move(previousId),
    };
    const TaskbarWidget::TaskModel next{
        .handleKey = 11,
        .exactCompositorWindowId = std::move(nextId),
    };
    return TaskbarWidget::compareModels(false, {previous}, {}, {next}, {}).layoutEqual;
  }
};

int main() {
  // A hidden title change keeps the layout and reports the title change for tooltip refreshes.
  assert(TaskbarWidgetTestAccess::compare(false, 11, 11, "old", "new") == std::pair(true, true));
  // A displayed title is layout, so it still forces a rebuild.
  assert(!TaskbarWidgetTestAccess::compare(true, 11, 11, "old", "new").first);
  assert(TaskbarWidgetTestAccess::compare(true, 11, 11, "same", "same") == std::pair(true, false));
  // Task identity changes rebuild regardless of the title.
  assert(!TaskbarWidgetTestAccess::compare(false, 11, 12, "old", "new").first);

  auto tasks = std::vector{
      TaskbarWidgetTestAccess::task(11, "first"),
      TaskbarWidgetTestAccess::task(12, "second"),
  };
  assert(TaskbarWidgetTestAccess::resolvedTitle(tasks, 0, 7, 7) == std::optional<std::string>("first"));
  // Retained references read the current model, not the model they were built from.
  tasks[0].title = "retitled";
  assert(TaskbarWidgetTestAccess::resolvedTitle(tasks, 0, 7, 7) == std::optional<std::string>("retitled"));
  assert(!TaskbarWidgetTestAccess::resolvedTitle(tasks, 2, 7, 7).has_value());
  assert(!TaskbarWidgetTestAccess::resolvedTitle(tasks, 0, 7, 8).has_value());

  // Workspace placement can be rebound while compositor actions retain the
  // authoritative ext-foreign-toplevel identifier.
  assert(TaskbarWidgetTestAccess::rebindWorkspaceWindow("41", "42") == std::pair(std::string("42"), std::string("41")));
  // Exact compositor identity also remains authoritative for later workspace
  // reconciliation instead of preserving the crossed mutable assignment.
  assert(TaskbarWidgetTestAccess::bindingWindowId("42", "41") == "41");
  assert(TaskbarWidgetTestAccess::bindingWindowId("42", "") == "42");
  assert(!TaskbarWidgetTestAccess::exactWindowIdChangeKeepsLayout("41", "42"));

  return 0;
}
