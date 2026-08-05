#include "dbus/upower/upower_service.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

  class TempTree {
  public:
    TempTree() {
      const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
      root = std::filesystem::temp_directory_path() / ("noctalia-charge-limit-" + std::to_string(stamp));
      std::filesystem::create_directories(root / "BAT0");
    }

    ~TempTree() {
      std::error_code error;
      std::filesystem::permissions(
          root / "BAT0" / "charge_control_end_threshold", std::filesystem::perms::owner_all,
          std::filesystem::perm_options::add, error
      );
      std::filesystem::remove_all(root, error);
    }

    void write(std::string_view name, std::string_view value) const {
      std::ofstream output(root / "BAT0" / name);
      output << value;
    }

    std::filesystem::path root;
  };

  UPowerChargeLimitState supportedState(bool enabled) {
    UPowerChargeLimitState state;
    state.capabilityAvailable = true;
    state.supported = true;
    state.methodAvailable = true;
    state.enabledAvailable = true;
    state.enabled = enabled;
    return state;
  }

} // namespace

int main() {
  TempTree tree;

  tree.write("charge_control_start_threshold", "75\n");
  tree.write("charge_control_end_threshold", " 80 \n");
  auto probe = readChargeThresholdsFromSysfs("BAT0", tree.root);
  assert(probe.nativePathValid);
  assert(probe.start == 75U);
  assert(probe.end == 80U);

  std::filesystem::remove(tree.root / "BAT0" / "charge_control_end_threshold");
  probe = readChargeThresholdsFromSysfs("BAT0", tree.root);
  assert(probe.start == 75U);
  assert(!probe.end.has_value());

  std::filesystem::remove(tree.root / "BAT0" / "charge_control_start_threshold");
  tree.write("charge_control_end_threshold", "85");
  probe = readChargeThresholdsFromSysfs("BAT0", tree.root);
  assert(!probe.start.has_value());
  assert(probe.end == 85U);

  probe = readChargeThresholdsFromSysfs("BAT1", tree.root);
  assert(probe.nativePathValid);
  assert(!probe.start.has_value() && !probe.end.has_value());

  for (const std::string value : {"nope", "80 percent", "-1", "101", "4294967296"}) {
    tree.write("charge_control_end_threshold", value);
    probe = readChargeThresholdsFromSysfs("BAT0", tree.root);
    assert(!probe.end.has_value());
  }

  tree.write("charge_control_end_threshold", "80");
  std::filesystem::permissions(
      tree.root / "BAT0" / "charge_control_end_threshold", std::filesystem::perms::none,
      std::filesystem::perm_options::replace
  );
  probe = readChargeThresholdsFromSysfs("BAT0", tree.root);
  if (geteuid() != 0) {
    assert(!probe.end.has_value());
  }

  for (const std::string unsafe :
       {"", ".", "..", "../BAT0", "BAT0/../../etc", "/sys/class/power_supply/BAT0", "BAT 0", "BAT0\\x"}) {
    probe = readChargeThresholdsFromSysfs(unsafe, tree.root);
    assert(!probe.nativePathValid);
    assert(!probe.start.has_value() && !probe.end.has_value());
  }

  UPowerChargeLimitState state;
  assert(classifyChargeLimit(state) == ChargeLimitMode::Unsupported);

  state = supportedState(true);
  state.supportedSettings = 3U;
  state.configuredStart = 75U;
  state.configuredEnd = 80U;
  state.effectiveStart = 75U;
  state.effectiveEnd = 80U;
  assert(chargeLimitIsRestrictive(state));
  assert(classifyChargeLimit(state) == ChargeLimitMode::UPowerActive);

  state = supportedState(false);
  state.effectiveStart = 0U;
  state.effectiveEnd = 100U;
  assert(!chargeLimitIsRestrictive(state));
  assert(classifyChargeLimit(state) == ChargeLimitMode::UPowerDisabled);

  state = supportedState(false);
  state.configuredStart = 75U;
  state.configuredEnd = 80U;
  state.effectiveStart = 75U;
  state.effectiveEnd = 80U;
  assert(classifyChargeLimit(state) == ChargeLimitMode::ExternallyManaged);

  state = supportedState(true);
  state.supportedSettings = 4U;
  assert(classifyChargeLimit(state) == ChargeLimitMode::FirmwareManaged);

  state = supportedState(true);
  state.supportedSettings = 2U;
  state.configuredEnd = 80U;
  state.effectiveEnd = 80U;
  assert(classifyChargeLimit(state) == ChargeLimitMode::UPowerActive);

  state.enabledAvailable = false;
  assert(classifyChargeLimit(state) == ChargeLimitMode::ReadOnly);

  state = {};
  state.effectiveEnd = 100U;
  assert(classifyChargeLimit(state) == ChargeLimitMode::ReadOnly);

  state = {};
  state.effectiveStart = 70U;
  assert(classifyChargeLimit(state) == ChargeLimitMode::ExternallyManaged);

  state = supportedState(true);
  state.configuredStart = 70U;
  state.configuredEnd = 80U;
  state.effectiveStart = 75U;
  state.effectiveEnd = 85U;
  state.requestPending = true;
  state.requestedEnabled = false;
  assert(state.configuredStart != state.effectiveStart);
  assert(state.configuredEnd != state.effectiveEnd);
  assert(classifyChargeLimit(state) == ChargeLimitMode::UPowerActive);
  assert(chargeLimitControlState(state) == (ChargeLimitControlState{true, false, false}));

  // A failed operation rolls back to refreshed UPower state and exposes a localized error category.
  state = supportedState(false);
  state.operationError = ChargeLimitOperationError::PermissionDenied;
  assert(chargeLimitControlState(state) == (ChargeLimitControlState{true, false, true}));

  // A successful refresh reconciles the checked state from UPower.
  state = supportedState(true);
  assert(chargeLimitControlState(state) == (ChargeLimitControlState{true, true, true}));

  state = supportedState(false);
  state.effectiveStart = 75U;
  state.effectiveEnd = 80U;
  assert(chargeLimitControlState(state) == (ChargeLimitControlState{false, false, false}));

  return 0;
}
