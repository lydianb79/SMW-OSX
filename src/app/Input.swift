import AppKit
import GameController
import Carbon.HIToolbox

/// Collects keyboard + game controller state and turns it into a SNES pad word.
final class InputManager {
  static let shared = InputManager()
  private var keysDown = Set<UInt16>()
  private var lastHot = Set<HotAction>()
  /// When set, the next key / gamepad button press is delivered here instead of the game.
  var captureKey: ((UInt16) -> Void)?
  var capturePad: ((String) -> Void)?
  var onHotAction: ((HotAction) -> Void)?

  private init() {
    NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { _ in }
    GCController.startWirelessControllerDiscovery(completionHandler: nil)
  }

  func keyDown(_ code: UInt16) {
    if let cap = captureKey { captureKey = nil; cap(code); return }
    keysDown.insert(code)
  }
  func keyUp(_ code: UInt16) { keysDown.remove(code) }
  func releaseAll() { keysDown.removeAll() }

  private func pressedPadNames() -> Set<String> {
    var names = Set<String>()
    for c in GCController.controllers() {
      let prof = c.physicalInputProfile
      for (name, b) in prof.buttons where b.isPressed { names.insert(name) }
    }
    return names
  }

  private func padDirections() -> UInt16 {
    var m: UInt16 = 0
    for c in GCController.controllers() {
      guard let gp = c.extendedGamepad else { continue }
      let dp = gp.dpad, st = gp.leftThumbstick
      if dp.up.isPressed || st.yAxis.value > 0.5 { m |= PadButton.up.mask }
      if dp.down.isPressed || st.yAxis.value < -0.5 { m |= PadButton.down.mask }
      if dp.left.isPressed || st.xAxis.value < -0.5 { m |= PadButton.left.mask }
      if dp.right.isPressed || st.xAxis.value > 0.5 { m |= PadButton.right.mask }
    }
    return m
  }

  /// Polls everything; returns the pad word and whether fast-forward is held.
  func poll() -> (UInt16, Bool) {
    let s = Settings.shared
    let padNames = pressedPadNames()
    if let cap = capturePad, let first = padNames.sorted().first(where: { !$0.hasPrefix("Direction Pad") && !$0.contains("Thumbstick") }) {
      capturePad = nil
      cap(first)
      return (0, false)
    }
    var word: UInt16 = padDirections()
    for b in PadButton.allCases {
      if let k = s.keyMap[b.rawValue], keysDown.contains(k) { word |= b.mask }
      if let n = s.padMap[b.rawValue], padNames.contains(n) { word |= b.mask }
    }
    var hot = Set<HotAction>()
    for h in HotAction.allCases {
      if let k = s.keyMap[h.rawValue], keysDown.contains(k) { hot.insert(h) }
      if let n = s.padMap[h.rawValue], padNames.contains(n) { hot.insert(h) }
    }
    for h in hot.subtracting(lastHot) where h != .fastForward { onHotAction?(h) }
    lastHot = hot
    return (word, hot.contains(.fastForward))
  }

  static func keyName(_ code: UInt16) -> String {
    let special: [Int: String] = [
      kVK_Return: "Return", kVK_Tab: "Tab", kVK_Space: "Space", kVK_Delete: "Delete", kVK_Escape: "Esc",
      kVK_Shift: "Left Shift", kVK_RightShift: "Right Shift", kVK_Control: "Left Ctrl", kVK_RightControl: "Right Ctrl",
      kVK_Option: "Left Option", kVK_RightOption: "Right Option", kVK_CapsLock: "Caps Lock",
      kVK_LeftArrow: "←", kVK_RightArrow: "→", kVK_UpArrow: "↑", kVK_DownArrow: "↓",
      kVK_F1: "F1", kVK_F2: "F2", kVK_F3: "F3", kVK_F4: "F4", kVK_F5: "F5", kVK_F6: "F6",
      kVK_F7: "F7", kVK_F8: "F8", kVK_F9: "F9", kVK_F10: "F10", kVK_F11: "F11", kVK_F12: "F12",
      kVK_Home: "Home", kVK_End: "End", kVK_PageUp: "Page Up", kVK_PageDown: "Page Down",
      kVK_ForwardDelete: "Fwd Delete", kVK_ANSI_KeypadEnter: "Keypad Enter",
    ]
    if let s = special[Int(code)] { return s }
    let src = TISCopyCurrentKeyboardLayoutInputSource().takeRetainedValue()
    guard let ptr = TISGetInputSourceProperty(src, kTISPropertyUnicodeKeyLayoutData) else { return "Key \(code)" }
    let data = Unmanaged<CFData>.fromOpaque(ptr).takeUnretainedValue() as Data
    var dead: UInt32 = 0
    var chars = [UniChar](repeating: 0, count: 4)
    var len = 0
    let status = data.withUnsafeBytes { raw -> OSStatus in
      let layout = raw.bindMemory(to: UCKeyboardLayout.self).baseAddress!
      return UCKeyTranslate(layout, code, UInt16(kUCKeyActionDisplay), 0, UInt32(LMGetKbdType()),
                            OptionBits(kUCKeyTranslateNoDeadKeysBit), &dead, 4, &len, &chars)
    }
    if status == noErr, len > 0 { return String(utf16CodeUnits: chars, count: len).uppercased() }
    return "Key \(code)"
  }
}
