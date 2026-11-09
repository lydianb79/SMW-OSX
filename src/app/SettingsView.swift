import SwiftUI
import AppKit
import GameController

struct SettingsView: View {
  @ObservedObject var s = Settings.shared
  var body: some View {
    TabView {
      InputTab().tabItem { Label("Input", systemImage: "gamecontroller") }
      GraphicsTab().tabItem { Label("Graphics", systemImage: "display") }
      AudioTab().tabItem { Label("Audio", systemImage: "speaker.wave.2") }
    }
    .padding(16)
    .frame(width: 640, height: 640)
  }
}

// MARK: - Input
private struct InputTab: View {
  @ObservedObject var s = Settings.shared
  @State private var capturing: String?
  @State private var controllers: [String] = []

  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      ScrollView {
        Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 6) {
          GridRow {
            Text("Button").font(.headline)
            Text("Keyboard").font(.headline)
            Text("Gamepad").font(.headline)
          }
          Divider().gridCellUnsizedAxes(.horizontal)
          ForEach(PadButton.allCases) { b in row(id: b.rawValue, label: b.label, padBindable: ![.up, .down, .left, .right].contains(b)) }
          Divider().gridCellUnsizedAxes(.horizontal)
          ForEach(HotAction.allCases) { h in row(id: h.rawValue, label: h.label, padBindable: true) }
        }
        .padding(.vertical, 4)
      }
      Text("D-pad and left stick always steer. Click a binding, then press the key or gamepad button to assign; Esc clears it.")
        .font(.caption).foregroundStyle(.secondary)
      HStack {
        Button("Reset Keyboard") { s.resetKeyboard() }
        Button("Reset Gamepad") { s.resetGamepad() }
        Spacer()
        Text(controllers.isEmpty ? "No controller connected" : "Connected: " + controllers.joined(separator: ", "))
          .font(.caption).foregroundStyle(.secondary)
      }
    }
    .onAppear(perform: refresh)
    .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { _ in refresh() }
    .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect)) { _ in refresh() }
    .onDisappear { cancelCapture() }
  }

  private func refresh() { controllers = GCController.controllers().compactMap { $0.vendorName } }

  private func cancelCapture() {
    InputManager.shared.captureKey = nil
    InputManager.shared.capturePad = nil
    capturing = nil
  }

  @ViewBuilder private func row(id: String, label: String, padBindable: Bool) -> some View {
    GridRow {
      Text(label).frame(width: 190, alignment: .leading)
      Button(action: {
        cancelCapture()
        capturing = "k:" + id
        InputManager.shared.captureKey = { code in
          if code == 53 { s.keyMap.removeValue(forKey: id) }       // Esc clears
          else {
            for (k, v) in s.keyMap where v == code && k != id { s.keyMap.removeValue(forKey: k) }
            s.keyMap[id] = code
          }
          capturing = nil
        }
      }) {
        Text(capturing == "k:" + id ? "Press a key…" : (s.keyMap[id].map(InputManager.keyName) ?? "—"))
          .frame(width: 130)
      }
      if padBindable {
        Button(action: {
          cancelCapture()
          capturing = "p:" + id
          InputManager.shared.captureKey = { code in
            if code == 53 { s.padMap.removeValue(forKey: id) }
            InputManager.shared.capturePad = nil
            capturing = nil
          }
          InputManager.shared.capturePad = { name in
            for (k, v) in s.padMap where v == name && k != id { s.padMap.removeValue(forKey: k) }
            s.padMap[id] = name
            InputManager.shared.captureKey = nil
            capturing = nil
          }
        }) {
          Text(capturing == "p:" + id ? "Press a button…" : (s.padMap[id] ?? "—")).frame(width: 150)
        }
      } else {
        Text("D-pad / Stick").foregroundStyle(.secondary).frame(width: 150)
      }
    }
  }
}

// MARK: - Graphics
private struct GraphicsTab: View {
  @ObservedObject var s = Settings.shared
  var body: some View {
    Form {
      Section {
        Picker("Filter", selection: $s.filter) { ForEach(VideoFilter.allCases) { Text($0.label).tag($0) } }
          .disabled(s.modernGraphics)
        Picker("Aspect ratio", selection: $s.aspect) { ForEach(AspectMode.allCases) { Text($0.label).tag($0) } }
        Toggle("Integer scaling", isOn: $s.integerScale)
      } header: {
        Text("Display")
      } footer: {
        if s.modernGraphics {
          Text("The filter is replaced by the modern upscaler while Modernized 2026 Graphics is on.")
            .font(.caption).foregroundStyle(.secondary)
        }
      }

      FullscreenSection()

      Section {
        HStack {
          Text("Preset")
          Spacer()
          ForEach(Settings.CRTPreset.allCases) { p in
            Button(p.rawValue) { s.apply(p) }.controlSize(.small)
          }
        }
        Picker("Scanlines", selection: $s.scanStyle) { ForEach(ScanlineStyle.allCases) { Text($0.label).tag($0) } }
        Group {
          Slider(value: $s.scanIntensity, in: 0...1) { Text("Scanline intensity") }
          Slider(value: $s.scanThickness, in: 0...1) { Text("Line thickness") }
        }
        .disabled(s.scanStyle == .off)
        Picker("Phosphor mask", selection: $s.maskType) { ForEach(MaskType.allCases) { Text($0.label).tag($0) } }
        Slider(value: $s.maskStrength, in: 0...1) { Text("Mask strength") }
          .disabled(s.maskType == .none)
        Slider(value: $s.scanBoost, in: 0...1) { Text("Brightness compensation") }
          .disabled(s.scanStyle == .off && s.maskType == .none)
      } header: {
        Text("Scanlines & CRT Mask")
      } footer: {
        Text("Applied on top of any filter, including Modernized 2026 Graphics. Masks and thin lines look best at 4× scale or more.")
          .font(.caption).foregroundStyle(.secondary)
      }

      Section {
        Toggle("Modernized 2026 Graphics", isOn: $s.modernGraphics)
        Group {
          Slider(value: $s.bloom, in: 0...1) { Text("Bloom / glow") }
          Slider(value: $s.vibrance, in: 0...1) { Text("Color vibrance") }
          Toggle("Ambient backdrop in borders", isOn: $s.ambientFill)
        }
        .disabled(!s.modernGraphics)
      } header: {
        Text("Modern")
      } footer: {
        Text("Edge-directed smooth upscaling of the original pixel art, HDR-style bloom, filmic color grading and a blurred ambient fill instead of black bars, all rendered live on the GPU.")
          .font(.caption).foregroundStyle(.secondary)
      }
    }
    .formStyle(.grouped)
  }
}

private struct FullscreenSection: View {
  @ObservedObject var s = Settings.shared
  @State private var modes: [DisplayModeOption] = []
  var body: some View {
    Section {
      Toggle("Start in full screen", isOn: $s.startFullscreen)
      Picker("Full screen resolution", selection: $s.fullscreenMode) {
        ForEach(modes) { Text($0.label).tag($0.id) }
        if !modes.contains(where: { $0.id == s.fullscreenMode }) {
          Text("Unavailable on this display").tag(s.fullscreenMode)
        }
      }
    } header: {
      Text("Full Screen")
    } footer: {
      Text("The display switches to this mode only while the game is full screen (⌃⌘F) and returns to your desktop resolution when you leave full screen or quit.")
        .font(.caption).foregroundStyle(.secondary)
    }
    .onAppear { modes = DisplayModes.available() }
    .onReceive(NotificationCenter.default.publisher(for: NSApplication.didChangeScreenParametersNotification)) { _ in
      if !DisplayModes.switched { modes = DisplayModes.available() }
    }
  }
}

// MARK: - Audio
private struct AudioTab: View {
  @ObservedObject var s = Settings.shared
  var body: some View {
    Form {
      Section("Output") {
        Slider(value: $s.volume, in: 0...1) { Text("Volume") }
        Toggle("Mute", isOn: $s.muted)
      }
      Section {
        Toggle("Modernized 2026 Audio", isOn: $s.modernAudio)
        Group {
          Slider(value: $s.reverb, in: 0...1) { Text("Room reverb") }
          Slider(value: $s.stereoWidth, in: 0...1) { Text("Stereo width") }
        }
        .disabled(!s.modernAudio)
      } header: {
        Text("Modern")
      } footer: {
        Text("Replaces the S-DSP's muffled Gaussian sample interpolation with crisp cubic interpolation, then masters the mix: DC filter, bass and air shelving EQ, mid/side stereo widening, a plate-style reverb and a soft-knee limiter.")
          .font(.caption).foregroundStyle(.secondary)
      }
    }
    .formStyle(.grouped)
  }
}
