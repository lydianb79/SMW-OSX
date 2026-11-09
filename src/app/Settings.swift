import Foundation
import Combine
import Carbon.HIToolbox

/// SNES controller buttons in the order shown in the settings UI.
enum PadButton: String, CaseIterable, Identifiable, Codable {
  case up, down, left, right, b, a, y, x, l, r, start, select
  var id: String { rawValue }
  var label: String {
    switch self {
    case .up: return "Up"; case .down: return "Down"; case .left: return "Left"; case .right: return "Right"
    case .b: return "B (Jump)"; case .a: return "A (Spin Jump)"; case .y: return "Y (Run / Fire)"
    case .x: return "X (Run / Fire)"; case .l: return "L"; case .r: return "R"
    case .start: return "Start"; case .select: return "Select"
    }
  }
  var mask: UInt16 {
    switch self {
    case .b: return UInt16(BTN_B); case .y: return UInt16(BTN_Y)
    case .select: return UInt16(BTN_SELECT); case .start: return UInt16(BTN_START)
    case .up: return UInt16(BTN_UP); case .down: return UInt16(BTN_DOWN)
    case .left: return UInt16(BTN_LEFT); case .right: return UInt16(BTN_RIGHT)
    case .a: return UInt16(BTN_A); case .x: return UInt16(BTN_X)
    case .l: return UInt16(BTN_L); case .r: return UInt16(BTN_R)
    }
  }
}

/// Extra actions bindable to keys/buttons.
enum HotAction: String, CaseIterable, Identifiable, Codable {
  case fastForward, quickSave, quickLoad
  var id: String { rawValue }
  var label: String {
    switch self {
    case .fastForward: return "Fast Forward (hold)"
    case .quickSave: return "Quick Save (current slot)"
    case .quickLoad: return "Quick Load (current slot)"
    }
  }
}

enum VideoFilter: String, CaseIterable, Identifiable {
  case nearest, bilinear, sharp, bicubic, lanczos, smoothEdges, lcd, composite
  var id: String { rawValue }
  var label: String {
    switch self {
    case .nearest: return "Nearest (pixel exact)"
    case .bilinear: return "Bilinear (soft)"
    case .sharp: return "Sharp Bilinear"
    case .bicubic: return "Bicubic (Catmull-Rom)"
    case .lanczos: return "Lanczos"
    case .smoothEdges: return "Smooth Edges (pixel-art AA)"
    case .lcd: return "LCD Grid"
    case .composite: return "Composite Video (color bleed)"
    }
  }
  var index: Int32 { Int32(VideoFilter.allCases.firstIndex(of: self)!) }
}

enum ScanlineStyle: String, CaseIterable, Identifiable {
  case off, sharp, beam
  var id: String { rawValue }
  var label: String {
    switch self {
    case .off: return "Off"
    case .sharp: return "Sharp lines"
    case .beam: return "Soft CRT beam"
    }
  }
  var index: Int32 { Int32(ScanlineStyle.allCases.firstIndex(of: self)!) }
}

enum MaskType: String, CaseIterable, Identifiable {
  case none, aperture, slot, shadow
  var id: String { rawValue }
  var label: String {
    switch self {
    case .none: return "None"
    case .aperture: return "Aperture grille"
    case .slot: return "Slot mask"
    case .shadow: return "Shadow mask (dots)"
    }
  }
  var index: Int32 { Int32(MaskType.allCases.firstIndex(of: self)!) }
}

enum AspectMode: String, CaseIterable, Identifiable {
  case tv43, pixel, stretch
  var id: String { rawValue }
  var label: String {
    switch self {
    case .tv43: return "4:3 (original TV)"
    case .pixel: return "8:7 (square pixels)"
    case .stretch: return "Stretch to window"
    }
  }
}

final class Settings: ObservableObject {
  static let shared = Settings()
  private let d = UserDefaults.standard
  private var bag = Set<AnyCancellable>()

  // graphics
  @Published var filter: VideoFilter
  @Published var aspect: AspectMode
  @Published var integerScale: Bool
  @Published var scanStyle: ScanlineStyle
  @Published var scanIntensity: Double
  @Published var scanThickness: Double
  @Published var maskType: MaskType
  @Published var maskStrength: Double
  @Published var scanBoost: Double
  @Published var modernGraphics: Bool
  @Published var bloom: Double
  @Published var vibrance: Double
  @Published var ambientFill: Bool
  @Published var startFullscreen: Bool
  @Published var fullscreenMode: String
  // audio
  @Published var modernAudio: Bool
  @Published var volume: Double
  @Published var reverb: Double
  @Published var stereoWidth: Double
  @Published var muted: Bool
  // input
  @Published var keyMap: [String: UInt16]          // PadButton/HotAction rawValue -> macOS keyCode
  @Published var padMap: [String: String]          // PadButton/HotAction rawValue -> GameController button name
  @Published var currentSlot: Int

  static let defaultKeys: [String: UInt16] = [
    PadButton.up.rawValue: UInt16(kVK_UpArrow), PadButton.down.rawValue: UInt16(kVK_DownArrow),
    PadButton.left.rawValue: UInt16(kVK_LeftArrow), PadButton.right.rawValue: UInt16(kVK_RightArrow),
    PadButton.b.rawValue: UInt16(kVK_ANSI_X), PadButton.a.rawValue: UInt16(kVK_ANSI_C),
    PadButton.y.rawValue: UInt16(kVK_ANSI_Z), PadButton.x.rawValue: UInt16(kVK_ANSI_S),
    PadButton.l.rawValue: UInt16(kVK_ANSI_Q), PadButton.r.rawValue: UInt16(kVK_ANSI_W),
    PadButton.start.rawValue: UInt16(kVK_Return), PadButton.select.rawValue: UInt16(kVK_RightShift),
    HotAction.fastForward.rawValue: UInt16(kVK_Tab),
    HotAction.quickSave.rawValue: UInt16(kVK_F5), HotAction.quickLoad.rawValue: UInt16(kVK_F9),
  ]
  static let defaultPad: [String: String] = [
    PadButton.b.rawValue: "Button A", PadButton.a.rawValue: "Button B",
    PadButton.y.rawValue: "Button X", PadButton.x.rawValue: "Button Y",
    PadButton.l.rawValue: "Left Shoulder", PadButton.r.rawValue: "Right Shoulder",
    PadButton.start.rawValue: "Button Menu", PadButton.select.rawValue: "Button Options",
    HotAction.fastForward.rawValue: "Right Trigger",
  ]

  private init() {
    filter = VideoFilter(rawValue: d.string(forKey: "filter") ?? "") ?? .sharp
    aspect = AspectMode(rawValue: d.string(forKey: "aspect") ?? "") ?? .tv43
    integerScale = d.object(forKey: "integerScale") as? Bool ?? false
    scanStyle = ScanlineStyle(rawValue: d.string(forKey: "scanStyle") ?? "") ?? .off
    scanIntensity = d.object(forKey: "scanIntensity") as? Double ?? 0.6
    scanThickness = d.object(forKey: "scanThickness") as? Double ?? 0.5
    maskType = MaskType(rawValue: d.string(forKey: "maskType") ?? "") ?? .none
    maskStrength = d.object(forKey: "maskStrength") as? Double ?? 0.4
    scanBoost = d.object(forKey: "scanBoost") as? Double ?? 0.3
    modernGraphics = d.object(forKey: "modernGraphics") as? Bool ?? false
    bloom = d.object(forKey: "bloom") as? Double ?? 0.4
    vibrance = d.object(forKey: "vibrance") as? Double ?? 0.25
    ambientFill = d.object(forKey: "ambientFill") as? Bool ?? true
    startFullscreen = d.object(forKey: "startFullscreen") as? Bool ?? false
    fullscreenMode = d.string(forKey: "fullscreenMode") ?? "native"
    modernAudio = d.object(forKey: "modernAudio") as? Bool ?? false
    volume = d.object(forKey: "volume") as? Double ?? 0.8
    reverb = d.object(forKey: "reverb") as? Double ?? 0.25
    stereoWidth = d.object(forKey: "stereoWidth") as? Double ?? 0.35
    muted = d.object(forKey: "muted") as? Bool ?? false
    keyMap = (d.dictionary(forKey: "keyMap") as? [String: UInt16]) ?? Settings.defaultKeys
    padMap = (d.dictionary(forKey: "padMap") as? [String: String]) ?? Settings.defaultPad
    currentSlot = d.object(forKey: "currentSlot") as? Int ?? 1
    for (k, v) in Settings.defaultKeys where keyMap[k] == nil { keyMap[k] = v }

    objectWillChange
      .debounce(for: .milliseconds(50), scheduler: RunLoop.main)
      .sink { [weak self] _ in self?.persist() }
      .store(in: &bag)
  }

  func persist() {
    d.set(filter.rawValue, forKey: "filter")
    d.set(aspect.rawValue, forKey: "aspect")
    d.set(integerScale, forKey: "integerScale")
    d.set(scanStyle.rawValue, forKey: "scanStyle")
    d.set(scanIntensity, forKey: "scanIntensity")
    d.set(scanThickness, forKey: "scanThickness")
    d.set(maskType.rawValue, forKey: "maskType")
    d.set(maskStrength, forKey: "maskStrength")
    d.set(scanBoost, forKey: "scanBoost")
    d.set(modernGraphics, forKey: "modernGraphics")
    d.set(bloom, forKey: "bloom")
    d.set(vibrance, forKey: "vibrance")
    d.set(ambientFill, forKey: "ambientFill")
    d.set(startFullscreen, forKey: "startFullscreen")
    d.set(fullscreenMode, forKey: "fullscreenMode")
    d.set(modernAudio, forKey: "modernAudio")
    d.set(volume, forKey: "volume")
    d.set(reverb, forKey: "reverb")
    d.set(stereoWidth, forKey: "stereoWidth")
    d.set(muted, forKey: "muted")
    d.set(keyMap, forKey: "keyMap")
    d.set(padMap, forKey: "padMap")
    d.set(currentSlot, forKey: "currentSlot")
  }

  enum CRTPreset: String, CaseIterable, Identifiable {
    case clean = "Clean", arcade = "Arcade Monitor", consumer = "Consumer TV", handheld = "LCD Handheld"
    var id: String { rawValue }
  }
  func apply(_ p: CRTPreset) {
    switch p {
    case .clean: scanStyle = .off; maskType = .none
    case .arcade: scanStyle = .beam; scanIntensity = 0.75; scanThickness = 0.55; maskType = .aperture; maskStrength = 0.45; scanBoost = 0.45
    case .consumer: scanStyle = .beam; scanIntensity = 0.45; scanThickness = 0.35; maskType = .slot; maskStrength = 0.35; scanBoost = 0.3
      if filter == .nearest || filter == .sharp { filter = .composite }
    case .handheld: scanStyle = .sharp; scanIntensity = 0.35; scanThickness = 0.3; maskType = .none; scanBoost = 0.15; filter = .lcd
    }
  }

  func resetKeyboard() { keyMap = Settings.defaultKeys }
  func resetGamepad() { padMap = Settings.defaultPad }
}
