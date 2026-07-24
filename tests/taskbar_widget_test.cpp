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

  static std::optional<std::string>
  currentTitle(std::vector<TaskbarWidget::TaskModel> tasks, const TaskbarWidget::TaskModel& identity) {
    const auto* current = TaskbarWidget::currentTask(tasks, identity);
    return current != nullptr && !current->title.empty() ? std::optional<std::string>(current->title) : std::nullopt;
  }

  static std::optional<std::string>
  currentAppId(std::vector<TaskbarWidget::TaskModel> tasks, const TaskbarWidget::TaskModel& identity) {
    const auto* current = TaskbarWidget::currentTask(tasks, identity);
    return current != nullptr ? std::optional<std::string>(current->appId) : std::nullopt;
  }

  static TaskbarWidget::TaskModel task(
      std::uintptr_t handleKey, std::string title, std::string windowId = {}, std::string desktopEntryId = {},
      bool pinned = false, std::string appId = {}
  ) {
    return {
        .handleKey = handleKey,
        .appId = std::move(appId),
        .title = std::move(title),
        .workspaceWindowId = std::move(windowId),
        .desktopEntryId = std::move(desktopEntryId),
        .pinned = pinned,
    };
  }
};

int main() {
  assert(TaskbarWidgetTestAccess::compare(false, 11, 11, "old", "new") == std::pair(true, true));
  assert(!TaskbarWidgetTestAccess::compare(true, 11, 11, "old", "new").first);
  assert(TaskbarWidgetTestAccess::compare(true, 11, 11, "same", "same") == std::pair(true, false));
  assert(!TaskbarWidgetTestAccess::compare(false, 11, 12, "old", "new").first);

  const auto original = TaskbarWidgetTestAccess::task(11, "old", "window-11");
  auto tasks = std::vector{
      TaskbarWidgetTestAccess::task(11, "updated", "window-11"),
      TaskbarWidgetTestAccess::task(12, "second", "window-12"),
  };
  assert(TaskbarWidgetTestAccess::currentTitle(tasks, original) == std::optional<std::string>("updated"));

  tasks[0].appId = "updated.app";
  assert(TaskbarWidgetTestAccess::currentAppId(tasks, original) == std::optional<std::string>("updated.app"));

  tasks[0].title.clear();
  assert(!TaskbarWidgetTestAccess::currentTitle(tasks, original).has_value());

  tasks[0] = TaskbarWidgetTestAccess::task(21, "renumbered", "window-11");
  assert(TaskbarWidgetTestAccess::currentTitle(tasks, original) == std::optional<std::string>("renumbered"));

  const auto pinned = TaskbarWidgetTestAccess::task(0, "old", {}, "dev.noctalia.Example", true);
  tasks = {TaskbarWidgetTestAccess::task(0, "updated", {}, "dev.noctalia.Example", true)};
  assert(TaskbarWidgetTestAccess::currentTitle(tasks, pinned) == std::optional<std::string>("updated"));

  return 0;
}
