import AppKit
import MetalKit
import SwiftUI
import Carbon.HIToolbox
import Combine
import UniformTypeIdentifiers

final class GameWindow: NSWindow {
  override var canBecomeKey: Bool { true }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate, NSMenuItemValidation, NSWindowDelegate {
  var window: GameWindow!
  var mtkView: MTKView!
  var renderer: Renderer!
  let emu = Emulator()
  var settingsWindow: NSWindow?
  var toast: NSTextField!
  var toastTimer: Timer?
  var slotMenu: NSMenu!
  var keyMonitor: Any?
  var modeWatch: AnyCancellable?

  func applicationDidFinishLaunching(_ note: Notification) {
    buildMenus()
    buildWindow()
    installKeyMonitor()
    InputManager.shared.onHotAction = { [weak self] a in
      guard let self else { return }
      switch a {
      case .quickSave: self.emu.saveSlot(Settings.shared.currentSlot)
      case .quickLoad: self.emu.loadSlot(Settings.shared.currentSlot)
      case .fastForward: break
      }
    }
    emu.onMessage = { [weak self] m in self?.showToast(m) }
    openInitialROM()
    NSApp.activate(ignoringOtherApps: true)
  }

  func applicationShouldTerminateAfterLastWindowClosed(_ s: NSApplication) -> Bool { true }
  func applicationWillTerminate(_ n: Notification) { DisplayModes.restore(); emu.flushSram(); Settings.shared.persist() }

  // MARK: window
  private func buildWindow() {
    let rect = NSRect(x: 0, y: 0, width: 1024, height: 768)
    window = GameWindow(contentRect: rect, styleMask: [.titled, .closable, .miniaturizable, .resizable],
                        backing: .buffered, defer: false)
    window.title = "SMW Recomp"
    window.collectionBehavior = [.fullScreenPrimary]
    window.contentMinSize = NSSize(width: 256, height: 224)
    window.delegate = self
    window.backgroundColor = .black
    window.setFrameAutosaveName("MainWindow")

    let dev = MTLCreateSystemDefaultDevice()!
    mtkView = MTKView(frame: rect, device: dev)
    mtkView.colorPixelFormat = .bgra8Unorm
    mtkView.preferredFramesPerSecond = 60
    mtkView.framebufferOnly = true
    mtkView.autoresizingMask = [.width, .height]
    renderer = Renderer(view: mtkView)
    renderer.emulator = emu
    mtkView.delegate = renderer

    let container = NSView(frame: rect)
    container.autoresizingMask = [.width, .height]
    container.addSubview(mtkView)
    toast = NSTextField(labelWithString: "")
    toast.font = .systemFont(ofSize: 15, weight: .semibold)
    toast.textColor = .white
    toast.wantsLayer = true
    toast.layer?.backgroundColor = NSColor.black.withAlphaComponent(0.6).cgColor
    toast.layer?.cornerRadius = 8
    toast.alphaValue = 0
    toast.translatesAutoresizingMaskIntoConstraints = false
    container.addSubview(toast)
    NSLayoutConstraint.activate([
      toast.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: 16),
      toast.topAnchor.constraint(equalTo: container.topAnchor, constant: 16),
    ])
    window.contentView = container
    window.center()
    window.makeKeyAndOrderFront(nil)
    window.makeFirstResponder(mtkView)
    DisplayModes.gameScreen = window.screen
    // re-apply the chosen resolution live when it changes while full screen
    modeWatch = Settings.shared.$fullscreenMode.dropFirst().receive(on: RunLoop.main).sink { [weak self] id in
      guard let self, self.window.styleMask.contains(.fullScreen) else { return }
      self.applyFullscreenMode(id)
    }
    if Settings.shared.startFullscreen {
      DispatchQueue.main.async { self.window.toggleFullScreen(nil) }
    }
  }

  func windowDidResignKey(_ n: Notification) { InputManager.shared.releaseAll() }

  // MARK: full screen resolution
  func windowDidChangeScreen(_ n: Notification) { DisplayModes.gameScreen = window.screen }
  func windowDidEnterFullScreen(_ n: Notification) { applyFullscreenMode(Settings.shared.fullscreenMode) }
  func windowWillExitFullScreen(_ n: Notification) { DisplayModes.restore() }

  private func applyFullscreenMode(_ id: String) {
    if !DisplayModes.apply(id: id, screen: window.screen) {
      showToast("Couldn't switch to that resolution")
    } else if id != DisplayModes.native {
      showToast("Full screen: " + (DisplayModes.available(screen: window.screen).first { $0.id == id }?.label ?? id))
    }
  }

  func showToast(_ text: String) {
    toast.stringValue = "  \(text)  "
    NSAnimationContext.runAnimationGroup { $0.duration = 0.15; toast.animator().alphaValue = 1 }
    toastTimer?.invalidate()
    toastTimer = Timer.scheduledTimer(withTimeInterval: 1.8, repeats: false) { [weak self] _ in
      NSAnimationContext.runAnimationGroup { $0.duration = 0.4; self?.toast.animator().alphaValue = 0 }
    }
  }

  // MARK: keyboard
  private func installKeyMonitor() {
    keyMonitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp, .flagsChanged]) { [weak self] ev in
      guard let self else { return ev }
      let input = InputManager.shared
      if ev.type == .keyDown, input.captureKey != nil {
        input.keyDown(ev.keyCode); return nil
      }
      if ev.type == .flagsChanged, input.captureKey != nil {
        let mods: [Int] = [kVK_Shift, kVK_RightShift, kVK_Control, kVK_RightControl, kVK_Option, kVK_RightOption]
        if mods.contains(Int(ev.keyCode)) { input.keyDown(ev.keyCode); return nil }
        return ev
      }
      guard ev.window === self.window else { return ev }
      if ev.modifierFlags.contains(.command) {
        if ev.type == .keyUp { input.keyUp(ev.keyCode) }
        return ev
      }
      switch ev.type {
      case .keyDown: if !ev.isARepeat { input.keyDown(ev.keyCode) }; return nil
      case .keyUp: input.keyUp(ev.keyCode); return nil
      case .flagsChanged:
        let flag: NSEvent.ModifierFlags?
        switch Int(ev.keyCode) {
        case kVK_Shift, kVK_RightShift: flag = .shift
        case kVK_Control, kVK_RightControl: flag = .control
        case kVK_Option, kVK_RightOption: flag = .option
        case kVK_CapsLock: flag = .capsLock
        default: flag = nil
        }
        if let f = flag {
          if ev.modifierFlags.contains(f) { input.keyDown(ev.keyCode) } else { input.keyUp(ev.keyCode) }
        }
        return ev
      default: return ev
      }
    }
  }

  // MARK: ROM
  private func openInitialROM() {
    let d = UserDefaults.standard
    if let path = d.string(forKey: "lastROM"), FileManager.default.fileExists(atPath: path), tryLoad(URL(fileURLWithPath: path)) { return }
    if let bundled = Bundle.main.url(forResource: "smw", withExtension: "smc"), tryLoad(bundled) { return }
    DispatchQueue.main.async { self.openROM(nil) }
  }

  @discardableResult private func tryLoad(_ url: URL) -> Bool {
    do {
      try emu.load(url: url)
      window.title = emu.romTitle.isEmpty ? "SMW Recomp" : "\(emu.romTitle) — SMW Recomp"
      if !url.path.hasPrefix(Bundle.main.bundlePath) { UserDefaults.standard.set(url.path, forKey: "lastROM") }
      NSDocumentController.shared.noteNewRecentDocumentURL(url)
      return true
    } catch {
      let a = NSAlert(error: error); a.runModal(); return false
    }
  }

  @objc func openROM(_ sender: Any?) {
    let p = NSOpenPanel()
    p.title = "Choose your Super Mario World ROM"
    p.allowedContentTypes = ["smc", "sfc", "swc", "fig"].compactMap { UTType(filenameExtension: $0) }
    p.allowsOtherFileTypes = true
    if p.runModal() == .OK, let url = p.url { tryLoad(url) }
  }

  func application(_ app: NSApplication, open urls: [URL]) { if let u = urls.first { tryLoad(u) } }

  @objc func resetSoft(_ s: Any?) { emu.reset(hard: false) }
  @objc func resetHard(_ s: Any?) { emu.reset(hard: true) }
  @objc func togglePause(_ s: Any?) { emu.paused.toggle(); showToast(emu.paused ? "Paused" : "Resumed") }

  // MARK: states
  @objc func saveCurrent(_ s: Any?) { emu.saveSlot(Settings.shared.currentSlot) }
  @objc func loadCurrent(_ s: Any?) { emu.loadSlot(Settings.shared.currentSlot) }
  @objc func selectSlot(_ s: NSMenuItem) { Settings.shared.currentSlot = s.tag; showToast("Slot \(s.tag) selected") }
  @objc func saveAs(_ s: Any?) {
    let p = NSSavePanel()
    p.nameFieldStringValue = "\(emu.romTitle.isEmpty ? "Game" : emu.romTitle).smwstate"
    if p.runModal() == .OK, let url = p.url { showToast(emu.saveState(to: url) ? "State saved" : "Save failed") }
  }
  @objc func loadFrom(_ s: Any?) {
    let p = NSOpenPanel()
    p.allowsOtherFileTypes = true
    if p.runModal() == .OK, let url = p.url { showToast(emu.loadState(from: url) ? "State loaded" : "Load failed") }
  }

  @objc func showSettings(_ s: Any?) {
    if settingsWindow == nil {
      let host = NSHostingController(rootView: SettingsView())
      let w = NSWindow(contentViewController: host)
      w.title = "Settings"
      w.styleMask = [.titled, .closable]
      w.isReleasedWhenClosed = false
      settingsWindow = w
      w.center()
    }
    settingsWindow?.makeKeyAndOrderFront(nil)
  }

  @objc func showAbout(_ s: Any?) {
    let credits = NSAttributedString(string: "Native macOS runtime for your own Super Mario World cartridge image.\n65C816 + SPC700 + S-DSP + PPU implemented in C, presented with Metal.",
                                     attributes: [.font: NSFont.systemFont(ofSize: 11)])
    NSApp.orderFrontStandardAboutPanel(options: [.credits: credits])
  }

  func validateMenuItem(_ item: NSMenuItem) -> Bool {
    let s = Settings.shared
    switch item.action {
    case #selector(togglePause(_:)): item.title = emu.paused ? "Resume" : "Pause"
    case #selector(selectSlot(_:)): item.state = s.currentSlot == item.tag ? .on : .off
    default: break
    }
    if [#selector(saveCurrent(_:)), #selector(loadCurrent(_:)), #selector(saveAs(_:)), #selector(loadFrom(_:)),
        #selector(resetSoft(_:)), #selector(resetHard(_:)), #selector(togglePause(_:))].contains(item.action) {
      return emu.snes != nil
    }
    return true
  }

  func menuNeedsUpdate(_ menu: NSMenu) {
    guard menu === slotMenu else { return }
    let f = DateFormatter(); f.dateStyle = .short; f.timeStyle = .short
    for item in menu.items {
      let date = emu.snes != nil ? emu.slotDate(item.tag) : nil
      item.title = "Slot \(item.tag)" + (date.map { " — " + f.string(from: $0) } ?? " — empty")
    }
  }

  // MARK: menus
  private func buildMenus() {
    let main = NSMenu()
    func add(_ menu: NSMenu, _ title: String, _ action: Selector?, _ key: String = "", _ mods: NSEvent.ModifierFlags = [.command], tag: Int = 0) -> NSMenuItem {
      let i = NSMenuItem(title: title, action: action, keyEquivalent: key)
      i.keyEquivalentModifierMask = mods
      i.tag = tag
      menu.addItem(i)
      return i
    }
    func top(_ title: String) -> NSMenu {
      let item = NSMenuItem(); let m = NSMenu(title: title); item.submenu = m; main.addItem(item); return m
    }
    let app = top("SMW Recomp")
    _ = add(app, "About SMW Recomp", #selector(showAbout(_:)))
    app.addItem(.separator())
    _ = add(app, "Settings…", #selector(showSettings(_:)), ",")
    app.addItem(.separator())
    _ = add(app, "Hide SMW Recomp", #selector(NSApplication.hide(_:)), "h")
    _ = add(app, "Hide Others", #selector(NSApplication.hideOtherApplications(_:)), "h", [.command, .option])
    _ = add(app, "Show All", #selector(NSApplication.unhideAllApplications(_:)))
    app.addItem(.separator())
    _ = add(app, "Quit SMW Recomp", #selector(NSApplication.terminate(_:)), "q")

    let file = top("Game")
    _ = add(file, "Open ROM…", #selector(openROM(_:)), "o")
    file.addItem(.separator())
    _ = add(file, "Pause", #selector(togglePause(_:)), "p")
    _ = add(file, "Reset", #selector(resetSoft(_:)), "r")
    _ = add(file, "Power Cycle", #selector(resetHard(_:)), "r", [.command, .shift])
    file.addItem(.separator())
    _ = add(file, "Close Window", #selector(NSWindow.performClose(_:)), "w")

    let st = top("State")
    _ = add(st, "Save State", #selector(saveCurrent(_:)), "s")
    _ = add(st, "Load State", #selector(loadCurrent(_:)), "l")
    let slotItem = NSMenuItem(title: "Slot", action: nil, keyEquivalent: "")
    slotMenu = NSMenu(title: "Slot")
    slotMenu.delegate = self
    for n in 1...9 { _ = add(slotMenu, "Slot \(n)", #selector(selectSlot(_:)), "\(n)", tag: n) }
    slotItem.submenu = slotMenu
    st.addItem(slotItem)
    st.addItem(.separator())
    _ = add(st, "Save State As…", #selector(saveAs(_:)), "s", [.command, .shift])
    _ = add(st, "Load State From…", #selector(loadFrom(_:)), "l", [.command, .shift])

    let view = top("View")
    _ = add(view, "Enter Full Screen", #selector(NSWindow.toggleFullScreen(_:)), "f", [.command, .control])

    let win = top("Window")
    _ = add(win, "Minimize", #selector(NSWindow.performMiniaturize(_:)), "m")
    _ = add(win, "Zoom", #selector(NSWindow.performZoom(_:)))
    NSApp.windowsMenu = win
    NSApp.mainMenu = main
  }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
NSWindow.allowsAutomaticWindowTabbing = false
app.run()
