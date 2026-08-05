#include "dbus/upower/upower_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"
#include "util/string_utils.h"
#include "util/sys_utils.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string_view>
#include <utility>
#include <vector>

namespace {

  const sdbus::ServiceName kUpowerBusName{"org.freedesktop.UPower"};
  const sdbus::ObjectPath kUpowerObjectPath{"/org/freedesktop/UPower"};
  constexpr auto kUpowerInterface = "org.freedesktop.UPower";
  constexpr auto kDeviceInterface = "org.freedesktop.UPower.Device";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
  constexpr auto kIntrospectableInterface = "org.freedesktop.DBus.Introspectable";

} // namespace

std::string batteryStateLabel(BatteryState state) {
  switch (state) {
  case BatteryState::Charging:
    return i18n::tr("power.battery.states.charging");
  case BatteryState::Discharging:
    return i18n::tr("power.battery.states.discharging");
  case BatteryState::FullyCharged:
    return i18n::tr("power.battery.states.plugged-in");
  case BatteryState::Empty:
    return i18n::tr("power.battery.states.empty");
  case BatteryState::PendingCharge:
    return i18n::tr("power.battery.states.plugged-in");
  case BatteryState::PendingDischarge:
    return i18n::tr("power.battery.states.pending-discharge");
  case BatteryState::Unknown:
  default:
    return i18n::tr("power.battery.states.battery");
  }
}

const char* batteryGlyphName(double percentage, BatteryState state) {
  if (state == BatteryState::Charging) {
    return "battery-charging";
  }
  if (state == BatteryState::FullyCharged || state == BatteryState::PendingCharge) {
    return "battery-plugged";
  }
  if (state == BatteryState::Unknown && percentage <= 0.0) {
    return "battery-exclamation";
  }
  if (percentage >= 85.0) {
    return "battery-4";
  }
  if (percentage >= 55.0) {
    return "battery-3";
  }
  if (percentage >= 30.0) {
    return "battery-2";
  }
  if (percentage >= 10.0) {
    return "battery-1";
  }
  return "battery-0";
}

const char* batteryDeviceGlyphName(UPowerDeviceType type) {
  switch (type) {
  case UPowerDeviceType::Mouse:
    return "mouse-2";
  case UPowerDeviceType::Keyboard:
    return "keyboard";
  case UPowerDeviceType::Phone:
  case UPowerDeviceType::Pda:
    return "device-mobile";
  default:
    return "bluetooth";
  }
}

namespace {

  template <typename T>
  T getPropertyOr(sdbus::IProxy& proxy, std::string_view iface, std::string_view propertyName, T fallback) {
    try {
      const sdbus::Variant value = proxy.getProperty(propertyName).onInterface(iface);
      return value.get<T>();
    } catch (const sdbus::Error&) {
      return fallback;
    }
  }

  template <typename T>
  std::optional<T> getOptionalProperty(sdbus::IProxy& proxy, std::string_view iface, std::string_view propertyName) {
    try {
      const sdbus::Variant value = proxy.getProperty(propertyName).onInterface(iface);
      return value.get<T>();
    } catch (const sdbus::Error&) {
      return std::nullopt;
    }
  }

  std::optional<std::uint32_t> thresholdProperty(sdbus::IProxy& proxy, std::string_view propertyName) {
    const auto value = getOptionalProperty<std::uint32_t>(proxy, kDeviceInterface, propertyName);
    if (!value.has_value() || *value > 100U) {
      return std::nullopt;
    }
    return value;
  }

  std::optional<std::uint32_t> readThresholdFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
      return std::nullopt;
    }

    std::string value;
    std::getline(input, value);
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      return std::nullopt;
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    const std::string_view trimmed(value.data() + first, last - first + 1);
    std::uint32_t threshold = 0;
    const auto [end, error] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), threshold);
    if (error != std::errc{} || end != trimmed.data() + trimmed.size() || threshold > 100U) {
      return std::nullopt;
    }
    return threshold;
  }

  bool isPlainPowerSupplyComponent(std::string_view value) {
    if (value.empty() || value == "." || value == "..") {
      return false;
    }
    return std::ranges::all_of(value, [](unsigned char ch) {
      return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.' || ch == ':';
    });
  }

  bool hasChargeThresholdMethod(sdbus::IProxy& proxy) {
    try {
      std::string xml;
      proxy.callMethod("Introspect").onInterface(kIntrospectableInterface).storeResultsTo(xml);
      return xml.find("<method name=\"EnableChargeThreshold\"") != std::string::npos
          || xml.find("<method name='EnableChargeThreshold'") != std::string::npos;
    } catch (const sdbus::Error&) {
      return false;
    }
  }

  bool isBatteryCapableDeviceType(UPowerDeviceType type) {
    return type != UPowerDeviceType::Unknown && type != UPowerDeviceType::LinePower;
  }

  // A battery belonging to a peripheral rather than to the system. UPower reports peripheral packs
  // with PowerSupply=false, so only PowerSupply batteries and UPS units power the machine itself.
  bool isPeripheralBattery(const UPowerDeviceInfo& info) {
    return info.isPresent
        && isBatteryCapableDeviceType(info.type)
        && !info.isLaptopBattery()
        && info.type != UPowerDeviceType::Ups;
  }

  bool isAutoSelector(std::string_view selector) {
    const std::string normalized = StringUtils::toLower(StringUtils::trim(selector));
    return normalized.empty() || normalized == "auto";
  }

  bool hasSelectorSuffix(std::string_view value, std::string_view selector) {
    if (value.empty() || selector.empty() || value.size() < selector.size()) {
      return false;
    }
    const std::size_t start = value.size() - selector.size();
    if (value.substr(start) != selector) {
      return false;
    }
    if (start == 0) {
      return true;
    }
    const char before = value[start - 1];
    return before == '/' || before == '_' || before == '-' || before == ':' || before == '.';
  }

  bool selectorMatchesField(const std::string& value, std::string_view selector) {
    return std::string_view(value) == selector || hasSelectorSuffix(value, selector);
  }

  BatteryState decodeBatteryState(std::uint32_t raw) {
    if (raw >= 1 && raw <= 6) {
      return static_cast<BatteryState>(raw);
    }
    return BatteryState::Unknown;
  }

  constexpr Logger kLog("upower");

  UPowerDeviceInfo makeDummyBatteryDevice() {
    UPowerDeviceInfo info;
    info.path = "/org/freedesktop/UPower/devices/dummy_battery";
    info.nativePath = "dummy_BAT0";
    info.model = "Dummy Battery";
    info.type = UPowerDeviceType::Battery;
    info.powerSupply = true;
    info.isPresent = true;
    info.energyFull = 54.0;
    info.energyFullDesign = 54.0;
    info.state.percentage = 67.0;
    info.state.energyRate = 12.5;
    info.state.state = BatteryState::Discharging;
    info.state.timeToEmpty = 3 * 3600 + 15 * 60;
    info.state.energy = 36.2;
    info.state.isPresent = true;
    info.state.onBattery = true;
    return info;
  }

} // namespace

ChargeThresholdProbe
readChargeThresholdsFromSysfs(std::string_view nativePath, const std::filesystem::path& powerSupplyRoot) {
  ChargeThresholdProbe result;
  if (!isPlainPowerSupplyComponent(nativePath)) {
    return result;
  }

  result.nativePathValid = true;
  const auto batteryPath = powerSupplyRoot / std::string(nativePath);
  result.start = readThresholdFile(batteryPath / "charge_control_start_threshold");
  result.end = readThresholdFile(batteryPath / "charge_control_end_threshold");
  return result;
}

bool chargeLimitIsRestrictive(const UPowerChargeLimitState& state) noexcept {
  return (state.effectiveStart.has_value() && *state.effectiveStart > 0U)
      || (state.effectiveEnd.has_value() && *state.effectiveEnd < 100U);
}

ChargeLimitMode classifyChargeLimit(const UPowerChargeLimitState& state) noexcept {
  if (!state.enabled && chargeLimitIsRestrictive(state)) {
    return ChargeLimitMode::ExternallyManaged;
  }

  if (state.capabilityAvailable && state.supported && state.methodAvailable && state.enabledAvailable) {
    if (!state.enabled) {
      return ChargeLimitMode::UPowerDisabled;
    }
    const bool firmware = state.supportedSettings.has_value() && ((*state.supportedSettings & 4U) != 0U);
    const bool hasNumericThreshold = state.configuredStart.has_value()
        || state.configuredEnd.has_value()
        || state.effectiveStart.has_value()
        || state.effectiveEnd.has_value();
    return firmware && !hasNumericThreshold ? ChargeLimitMode::FirmwareManaged : ChargeLimitMode::UPowerActive;
  }

  if (state.effectiveStart.has_value() || state.effectiveEnd.has_value()) {
    return ChargeLimitMode::ReadOnly;
  }
  return ChargeLimitMode::Unsupported;
}

ChargeLimitControlState chargeLimitControlState(const UPowerChargeLimitState& state) noexcept {
  const ChargeLimitMode mode = classifyChargeLimit(state);
  ChargeLimitControlState control;
  control.visible = mode == ChargeLimitMode::UPowerActive
      || mode == ChargeLimitMode::UPowerDisabled
      || mode == ChargeLimitMode::FirmwareManaged;
  control.checked = state.requestedEnabled.value_or(state.enabled);
  control.enabled = control.visible && !state.requestPending;
  return control;
}

bool upowerDeviceMatchesSelector(const UPowerDeviceInfo& info, std::string_view selector) {
  const std::string trimmed = StringUtils::trim(selector);
  if (trimmed.empty()) {
    return false;
  }
  return selectorMatchesField(info.path, trimmed)
      || selectorMatchesField(info.nativePath, trimmed)
      || selectorMatchesField(info.model, trimmed)
      || selectorMatchesField(info.serial, trimmed)
      || selectorMatchesField(info.vendor, trimmed);
}

UPowerService::UPowerService(SystemBus& bus) : m_bus(bus) {
  m_upowerProxy = sdbus::createProxy(m_bus.connection(), kUpowerBusName, kUpowerObjectPath);

  m_upowerProxy->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([this](
                const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& /*changed*/,
                const std::vector<std::string>& /*invalidated*/
            ) {
        if (interfaceName == kUpowerInterface) {
          refresh();
        }
      });

  m_upowerProxy->uponSignal("DeviceAdded").onInterface(kUpowerInterface).call([this](const sdbus::ObjectPath&) {
    rescanDevices();
  });

  m_upowerProxy->uponSignal("DeviceRemoved").onInterface(kUpowerInterface).call([this](const sdbus::ObjectPath&) {
    rescanDevices();
  });

  if (SysUtils::isEnvFlagOn("NOCTALIA_DUMMY_BATTERY")) {
    m_dummyDevice = makeDummyBatteryDevice();
    kLog.info("dummy battery enabled ({:.0F}% discharging)", m_dummyDevice->state.percentage);
  }

  rescanDevices();

  if (m_state.isPresent) {
    kLog.info(
        "battery {:.0F}% state={} ({})", m_state.percentage, static_cast<int>(m_state.state),
        m_state.onBattery ? "on battery" : "on AC"
    );
  } else {
    kLog.info("connected (no system battery present)");
  }
}

UPowerService::~UPowerService() { m_lifetimeToken.reset(); }

void UPowerService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void UPowerService::refresh() { refreshDeviceStates(); }

std::vector<UPowerDeviceInfo> UPowerService::batteryDevices() const {
  std::vector<UPowerDeviceInfo> devices;
  devices.reserve(m_devices.size() + (m_dummyDevice ? 1 : 0));
  for (const auto& device : m_devices) {
    if (device.info.isPresent && isBatteryCapableDeviceType(device.info.type)) {
      devices.push_back(device.info);
    }
  }
  if (m_dummyDevice && m_dummyDevice->isPresent) {
    devices.push_back(*m_dummyDevice);
  }
  return devices;
}

UPowerState UPowerService::stateForDevice(std::string_view selector) const {
  if (isAutoSelector(selector)) {
    return m_state;
  }

  if (const auto* device = deviceForSelector(selector); device != nullptr) {
    return device->state;
  }

  UPowerState missing;
  missing.onBattery = getPropertyOr<bool>(*m_upowerProxy, kUpowerInterface, "OnBattery", false);
  return missing;
}

void UPowerService::rescanDevices() {
  refreshDisplayDeviceProxy();

  std::vector<sdbus::ObjectPath> paths;
  try {
    m_upowerProxy->callMethod("EnumerateDevices").onInterface(kUpowerInterface).storeResultsTo(paths);
  } catch (const sdbus::Error& e) {
    kLog.warn("EnumerateDevices failed: {}", e.what());
    emitChangedIfNeeded(false);
    return;
  }

  std::vector<TrackedDevice> nextDevices;
  nextDevices.reserve(paths.size());
  for (const auto& path : paths) {
    try {
      auto proxy = sdbus::createProxy(m_bus.connection(), kUpowerBusName, path);
      auto info = readDeviceInfo(std::string(path), *proxy);
      if (!isBatteryCapableDeviceType(info.type)) {
        continue;
      }

      proxy->uponSignal("PropertiesChanged")
          .onInterface(kPropertiesInterface)
          .call([this](
                    const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& /*changed*/,
                    const std::vector<std::string>& /*invalidated*/
                ) {
            if (interfaceName == kDeviceInterface) {
              refresh();
            }
          });

      nextDevices.push_back(TrackedDevice{std::move(info), std::shared_ptr<sdbus::IProxy>(std::move(proxy))});
    } catch (const sdbus::Error&) {
      continue;
    }
  }

  std::ranges::sort(nextDevices, [](const TrackedDevice& lhs, const TrackedDevice& rhs) {
    return lhs.info.path < rhs.info.path;
  });

  for (auto& next : nextDevices) {
    const auto previous = std::ranges::find_if(m_devices, [&next](const TrackedDevice& device) {
      return device.info.path == next.info.path;
    });
    if (previous != m_devices.end()) {
      next.info.chargeLimit.requestPending = previous->info.chargeLimit.requestPending;
      next.info.chargeLimit.requestedEnabled = previous->info.chargeLimit.requestedEnabled;
      next.info.chargeLimit.operationError = previous->info.chargeLimit.operationError;
    }
  }

  bool devicesChanged = m_devices.size() != nextDevices.size();
  if (!devicesChanged) {
    for (std::size_t i = 0; i < m_devices.size(); ++i) {
      if (m_devices[i].info != nextDevices[i].info) {
        devicesChanged = true;
        break;
      }
    }
  }
  m_devices = std::move(nextDevices);
  if (devicesChanged) {
    kLog.debug("tracking {} UPower battery-capable device(s)", m_devices.size());
  }
  emitChangedIfNeeded(devicesChanged);
}

UPowerState UPowerService::readDefaultState() const {
  UPowerState next;

  next.onBattery = getPropertyOr<bool>(*m_upowerProxy, kUpowerInterface, "OnBattery", false);

  if (m_displayDeviceProxy != nullptr) {
    next = readDeviceState(*m_displayDeviceProxy);
    next.onBattery = getPropertyOr<bool>(*m_upowerProxy, kUpowerInterface, "OnBattery", false);
    if (next.isPresent) {
      return next;
    }
  }

  const auto* device = defaultSystemBattery();
  if (device == nullptr) {
    return next;
  }

  next = device->state;
  if (m_dummyDevice && device == &*m_dummyDevice) {
    return next;
  }
  next.onBattery = getPropertyOr<bool>(*m_upowerProxy, kUpowerInterface, "OnBattery", false);
  return next;
}

UPowerState UPowerService::readDeviceState(sdbus::IProxy& proxy) const {
  UPowerState next;

  next.onBattery = getPropertyOr<bool>(*m_upowerProxy, kUpowerInterface, "OnBattery", false);
  next.percentage = getPropertyOr<double>(proxy, kDeviceInterface, "Percentage", 0.0);
  next.isPresent = getPropertyOr<bool>(proxy, kDeviceInterface, "IsPresent", false);
  const auto rawState = getPropertyOr<std::uint32_t>(proxy, kDeviceInterface, "State", 0);
  next.state = decodeBatteryState(rawState);
  next.timeToEmpty = getPropertyOr<std::int64_t>(proxy, kDeviceInterface, "TimeToEmpty", 0);
  next.timeToFull = getPropertyOr<std::int64_t>(proxy, kDeviceInterface, "TimeToFull", 0);
  next.energyRate = getPropertyOr<double>(proxy, kDeviceInterface, "EnergyRate", 0.0);
  next.energy = getPropertyOr<double>(proxy, kDeviceInterface, "Energy", 0.0);

  // Fallback calculation for timeToEmpty / timeToFull if they are reported as 0 or less
  if (next.state == BatteryState::Discharging && next.timeToEmpty <= 0 && next.energyRate > 0.0 && next.energy > 0.0) {
    next.timeToEmpty = static_cast<std::int64_t>(std::round((next.energy / next.energyRate) * 3600.0));
  } else if (next.state == BatteryState::Charging && next.timeToFull <= 0 && next.energyRate > 0.0) {
    const auto energyFull = getPropertyOr<double>(proxy, kDeviceInterface, "EnergyFull", 0.0);
    if (energyFull > next.energy) {
      next.timeToFull = static_cast<std::int64_t>(std::round(((energyFull - next.energy) / next.energyRate) * 3600.0));
    }
  }

  return next;
}

UPowerDeviceInfo UPowerService::readDeviceInfo(
    std::string path, sdbus::IProxy& proxy, std::optional<bool> chargeThresholdMethodAvailable
) const {
  UPowerDeviceInfo info;
  info.path = std::move(path);
  info.nativePath = getPropertyOr<std::string>(proxy, kDeviceInterface, "NativePath", "");
  info.vendor = getPropertyOr<std::string>(proxy, kDeviceInterface, "Vendor", "");
  info.model = getPropertyOr<std::string>(proxy, kDeviceInterface, "Model", "");
  info.serial = getPropertyOr<std::string>(proxy, kDeviceInterface, "Serial", "");
  info.type = static_cast<UPowerDeviceType>(getPropertyOr<std::uint32_t>(proxy, kDeviceInterface, "Type", 0));
  info.powerSupply = getPropertyOr<bool>(proxy, kDeviceInterface, "PowerSupply", false);
  info.energyFull = getPropertyOr<double>(proxy, kDeviceInterface, "EnergyFull", 0.0);
  info.energyFullDesign = getPropertyOr<double>(proxy, kDeviceInterface, "EnergyFullDesign", 0.0);
  info.state = readDeviceState(proxy);
  info.isPresent = info.state.isPresent;

  const auto supported = getOptionalProperty<bool>(proxy, kDeviceInterface, "ChargeThresholdSupported");
  info.chargeLimit.capabilityAvailable = supported.has_value();
  info.chargeLimit.supported = supported.value_or(false);
  info.chargeLimit.methodAvailable = info.chargeLimit.supported
      && (chargeThresholdMethodAvailable.has_value() ? *chargeThresholdMethodAvailable
                                                     : hasChargeThresholdMethod(proxy));
  const auto enabled = getOptionalProperty<bool>(proxy, kDeviceInterface, "ChargeThresholdEnabled");
  info.chargeLimit.enabledAvailable = enabled.has_value();
  info.chargeLimit.enabled = enabled.value_or(false);
  info.chargeLimit.supportedSettings =
      getOptionalProperty<std::uint32_t>(proxy, kDeviceInterface, "ChargeThresholdSettingsSupported");
  if (!info.chargeLimit.supportedSettings.has_value() || (*info.chargeLimit.supportedSettings & 1U) != 0U) {
    info.chargeLimit.configuredStart = thresholdProperty(proxy, "ChargeStartThreshold");
  }
  if (!info.chargeLimit.supportedSettings.has_value() || (*info.chargeLimit.supportedSettings & 2U) != 0U) {
    info.chargeLimit.configuredEnd = thresholdProperty(proxy, "ChargeEndThreshold");
  }
  const auto effective = readChargeThresholdsFromSysfs(info.nativePath);
  info.chargeLimit.effectivePathValid = effective.nativePathValid;
  info.chargeLimit.effectiveStart = effective.start;
  info.chargeLimit.effectiveEnd = effective.end;
  return info;
}

bool UPowerService::enableChargeThreshold(std::string_view devicePath, bool enabled) {
  const auto it = std::ranges::find_if(m_devices, [devicePath](const TrackedDevice& device) {
    return device.info.path == devicePath;
  });
  if (it == m_devices.end()
      || !it->info.isLaptopBattery()
      || !it->info.chargeLimit.supported
      || !it->info.chargeLimit.methodAvailable
      || !it->info.chargeLimit.enabledAvailable
      || classifyChargeLimit(it->info.chargeLimit) == ChargeLimitMode::ExternallyManaged
      || it->info.chargeLimit.requestPending) {
    return false;
  }

  const std::string path = it->info.path;
  const auto proxy = it->proxy;
  auto& operation = it->info.chargeLimit;
  operation.requestPending = true;
  operation.requestedEnabled = enabled;
  operation.operationError = ChargeLimitOperationError::None;
  if (m_changeCallback) {
    m_changeCallback();
  }

  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  try {
    proxy->callMethodAsync("EnableChargeThreshold")
        .onInterface(kDeviceInterface)
        .withArguments(enabled)
        .uponReplyInvoke([this, lifetimeToken, path, keepAlive = proxy](std::optional<sdbus::Error> error) {
          (void)keepAlive;
          if (lifetimeToken.expired()) {
            return;
          }

          const auto current = std::ranges::find_if(m_devices, [&path](const TrackedDevice& device) {
            return device.info.path == path;
          });
          if (current == m_devices.end()) {
            return;
          }

          if (error.has_value()) {
            const auto& name = error->getName();
            const bool denied = name == sdbus::Error::Name{"org.freedesktop.DBus.Error.AccessDenied"}
                || name == sdbus::Error::Name{"org.freedesktop.PolicyKit1.Error.NotAuthorized"}
                || name == sdbus::Error::Name{"org.freedesktop.UPower.Device.PermissionDenied"};
            current->info.chargeLimit.requestPending = false;
            current->info.chargeLimit.requestedEnabled.reset();
            current->info.chargeLimit.operationError =
                denied ? ChargeLimitOperationError::PermissionDenied : ChargeLimitOperationError::Failed;
            kLog.warn("charge threshold change failed device={} err={}", path, error->what());
            if (m_changeCallback) {
              m_changeCallback();
            }
            return;
          }

          // Preserve completion state across the property refresh, then reconcile from UPower.
          refreshDeviceStates();
          const auto refreshed = std::ranges::find_if(m_devices, [&path](const TrackedDevice& device) {
            return device.info.path == path;
          });
          if (refreshed != m_devices.end()) {
            refreshed->info.chargeLimit.requestPending = false;
            refreshed->info.chargeLimit.requestedEnabled.reset();
            refreshed->info.chargeLimit.operationError = ChargeLimitOperationError::None;
            if (m_changeCallback) {
              m_changeCallback();
            }
          }
        });
  } catch (const sdbus::Error& error) {
    const auto current =
        std::ranges::find_if(m_devices, [&path](const TrackedDevice& device) { return device.info.path == path; });
    if (current != m_devices.end()) {
      current->info.chargeLimit.requestPending = false;
      current->info.chargeLimit.requestedEnabled.reset();
      current->info.chargeLimit.operationError = ChargeLimitOperationError::Failed;
    }
    kLog.warn("charge threshold change dispatch failed device={} err={}", path, error.what());
    if (m_changeCallback) {
      m_changeCallback();
    }
    return false;
  }
  return true;
}

const UPowerDeviceInfo* UPowerService::defaultSystemBattery() const noexcept {
  for (const auto& device : m_devices) {
    if (device.info.isLaptopBattery() && device.info.isPresent) {
      return &device.info;
    }
  }
  if (m_dummyDevice && m_dummyDevice->isLaptopBattery() && m_dummyDevice->isPresent) {
    return &*m_dummyDevice;
  }
  return nullptr;
}

const UPowerDeviceInfo* UPowerService::deviceForSelector(std::string_view selector) const {
  const std::string trimmed = StringUtils::trim(selector);
  if (trimmed.empty()) {
    return nullptr;
  }

  for (const auto& device : m_devices) {
    if (isBatteryCapableDeviceType(device.info.type) && upowerDeviceMatchesSelector(device.info, trimmed)) {
      return &device.info;
    }
  }
  if (m_dummyDevice
      && isBatteryCapableDeviceType(m_dummyDevice->type)
      && upowerDeviceMatchesSelector(*m_dummyDevice, trimmed)) {
    return &*m_dummyDevice;
  }
  return nullptr;
}

const UPowerDeviceInfo* UPowerService::peripheralBatteryForSerial(std::string_view serial) const {
  if (serial.empty()) {
    return nullptr;
  }
  for (const auto& device : m_devices) {
    if (isPeripheralBattery(device.info) && StringUtils::equalsInsensitive(device.info.serial, serial)) {
      return &device.info;
    }
  }
  return nullptr;
}

void UPowerService::refreshDisplayDeviceProxy() {
  sdbus::ObjectPath path;
  try {
    m_upowerProxy->callMethod("GetDisplayDevice").onInterface(kUpowerInterface).storeResultsTo(path);
  } catch (const sdbus::Error& e) {
    kLog.warn("GetDisplayDevice failed: {}", e.what());
    m_displayDeviceProxy.reset();
    m_displayDevicePath.clear();
    return;
  }

  const std::string nextPath(path);
  if (nextPath.empty() || nextPath == "/") {
    m_displayDeviceProxy.reset();
    m_displayDevicePath.clear();
    return;
  }
  if (m_displayDeviceProxy != nullptr && m_displayDevicePath == nextPath) {
    return;
  }

  try {
    auto proxy = sdbus::createProxy(m_bus.connection(), kUpowerBusName, path);
    proxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this](
                  const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& /*changed*/,
                  const std::vector<std::string>& /*invalidated*/
              ) {
          if (interfaceName == kDeviceInterface) {
            refresh();
          }
        });

    m_displayDevicePath = nextPath;
    m_displayDeviceProxy = std::move(proxy);
  } catch (const sdbus::Error& e) {
    kLog.warn("failed to track UPower display device {}: {}", nextPath, e.what());
    m_displayDeviceProxy.reset();
    m_displayDevicePath.clear();
  }
}

void UPowerService::refreshDeviceStates() {
  bool devicesChanged = false;
  for (auto& device : m_devices) {
    const std::optional<bool> knownMethod =
        device.info.chargeLimit.methodAvailable ? std::optional<bool>{true} : std::nullopt;
    auto next = readDeviceInfo(device.info.path, *device.proxy, knownMethod);
    next.chargeLimit.requestPending = device.info.chargeLimit.requestPending;
    next.chargeLimit.requestedEnabled = device.info.chargeLimit.requestedEnabled;
    next.chargeLimit.operationError = device.info.chargeLimit.operationError;
    if (next != device.info) {
      device.info = std::move(next);
      devicesChanged = true;
    }
  }
  emitChangedIfNeeded(devicesChanged);
}

void UPowerService::emitChangedIfNeeded(bool devicesChanged) {
  const UPowerState next = readDefaultState();
  if (!devicesChanged && next == m_state) {
    return;
  }

  m_state = next;
  if (m_changeCallback) {
    m_changeCallback();
  }
}
