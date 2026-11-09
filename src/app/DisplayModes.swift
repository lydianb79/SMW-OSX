import AppKit
import CoreGraphics

/// A selectable full-screen display mode.
struct DisplayModeOption: Hashable, Identifiable {
  let id: String
  let label: String
}

/// Lists and switches the hardware display modes used while the game is full screen.
enum DisplayModes {
  static let native = "native"
  /// Screen the game window lives on; set by the app delegate.
  static weak var gameScreen: NSScreen?

  static func displayID(for screen: NSScreen?) -> CGDirectDisplayID {
    let s = screen ?? gameScreen ?? NSScreen.main
    if let n = s?.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber {
      return CGDirectDisplayID(n.uint32Value)
    }
    return CGMainDisplayID()
  }

  private static func key(_ m: CGDisplayMode) -> String {
    "\(m.width)x\(m.height)x\(m.pixelWidth)@\(Int(m.refreshRate.rounded()))"
  }

  private static func allModes(_ d: CGDirectDisplayID) -> [CGDisplayMode] {
    let opts = [kCGDisplayShowDuplicateLowResolutionModes: kCFBooleanTrue] as CFDictionary
    let list = (CGDisplayCopyAllDisplayModes(d, opts) as? [CGDisplayMode]) ?? []
    return list.filter { $0.isUsableForDesktopGUI() }
  }

  /// Unique modes for the game's display, largest first.
  static func available(screen: NSScreen? = nil) -> [DisplayModeOption] {
    let d = displayID(for: screen)
    var seen = Set<String>()
    var out: [(CGDisplayMode, DisplayModeOption)] = []
    for m in allModes(d) {
      let k = key(m)
      if seen.contains(k) { continue }
      seen.insert(k)
      var label = "\(m.width) × \(m.height)"
      if m.pixelWidth > m.width { label += " (HiDPI)" }
      if m.refreshRate > 0 { label += " @ \(Int(m.refreshRate.rounded())) Hz" }
      out.append((m, DisplayModeOption(id: k, label: label)))
    }
    out.sort { a, b in
      if a.0.width != b.0.width { return a.0.width > b.0.width }
      if a.0.height != b.0.height { return a.0.height > b.0.height }
      if a.0.pixelWidth != b.0.pixelWidth { return a.0.pixelWidth > b.0.pixelWidth }
      return a.0.refreshRate > b.0.refreshRate
    }
    return [DisplayModeOption(id: native, label: "Native (desktop resolution)")] + out.map { $0.1 }
  }

  private(set) static var switched = false

  /// Switches the display to the mode with `id`; "native" restores the desktop mode.
  @discardableResult
  static func apply(id: String, screen: NSScreen?) -> Bool {
    restore()
    guard id != native else { return true }
    let d = displayID(for: screen)
    guard let mode = allModes(d).first(where: { key($0) == id }) else { return false }
    if let cur = CGDisplayCopyDisplayMode(d), key(cur) == id { return true }
    var cfg: CGDisplayConfigRef?
    guard CGBeginDisplayConfiguration(&cfg) == .success, let cfg else { return false }
    CGConfigureDisplayWithDisplayMode(cfg, d, mode, nil)
    // .forAppOnly: macOS reverts automatically if the app quits or crashes
    let ok = CGCompleteDisplayConfiguration(cfg, .forAppOnly) == .success
    switched = ok
    return ok
  }

  static func restore() {
    guard switched else { return }
    CGRestorePermanentDisplayConfiguration()
    switched = false
  }
}
