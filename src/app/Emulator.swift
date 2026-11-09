import Foundation
import AVFoundation
import CryptoKit
import QuartzCore

/// Owns the machine, drives frames, audio, battery saves and save states.
final class Emulator {
  private(set) var snes: UnsafeMutablePointer<Snes>?
  private(set) var romURL: URL?
  private(set) var romTitle = ""
  private(set) var romID = ""
  var paused = false
  var onMessage: ((String) -> Void)?

  private let engine = AVAudioEngine()
  private var sourceNode: AVAudioSourceNode?
  private var lastSram = Data()
  private var frameCounter = 0

  static let supportDir: URL = {
    let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
    let dir = base.appendingPathComponent("SMW Recomp", isDirectory: true)
    try? FileManager.default.createDirectory(at: dir.appendingPathComponent("Saves"), withIntermediateDirectories: true)
    try? FileManager.default.createDirectory(at: dir.appendingPathComponent("States"), withIntermediateDirectories: true)
    return dir
  }()

  init() { startAudio() }

  // MARK: ROM
  func load(url: URL) throws {
    let data = try Data(contentsOf: url)
    flushSram()
    if let s = snes { snes_destroy(s); snes = nil }
    var title = [CChar](repeating: 0, count: 32)
    let s: UnsafeMutablePointer<Snes>? = data.withUnsafeBytes { raw in
      snes_create(raw.bindMemory(to: UInt8.self).baseAddress, data.count, &title, title.count)
    }
    guard let s else { throw NSError(domain: "SMWRecomp", code: 1, userInfo: [NSLocalizedDescriptionKey: "Not a valid SNES ROM image."]) }
    snes = s
    romURL = url
    romTitle = String(cString: title).capitalized
    romID = Insecure.SHA1.hash(data: data).map { String(format: "%02x", $0) }.joined().prefix(12).description
    loadSram()
    ao_clear()
    paused = false
  }

  func reset(hard: Bool) {
    guard let s = snes else { return }
    flushSram()
    snes_reset(s, hard)
    loadSram()
    ao_clear()
    onMessage?(hard ? "Power cycled" : "Reset")
  }

  // MARK: frame loop (called once per display refresh)
  func tick() {
    let (pad, ff) = InputManager.shared.poll()
    let set = Settings.shared
    ao_set_params(set.modernAudio ? 1 : 0, Float(set.volume), Float(set.reverb), Float(set.stereoWidth), set.muted ? 1 : 0)
    guard let s = snes, !paused else { return }
    snes_set_pad(s, 0, pad)
    s.pointee.interp_mode = set.modernAudio ? 1 : 0
    let frames = ff ? 4 : 1
    for i in 0..<frames {
      snes_run_frame(s)
      if !ff || i == frames - 1 || ao_fill() < 800 { ao_push(s.pointee.audio_out, s.pointee.audio_count) }
    }
    frameCounter += 1
    if frameCounter % 300 == 0 { flushSram() }
  }

  // MARK: audio
  private func startAudio() {
    let out = engine.outputNode
    var rate = out.outputFormat(forBus: 0).sampleRate
    if rate < 8000 { rate = 48000 }
    ao_init(rate)
    guard let fmt = AVAudioFormat(standardFormatWithSampleRate: rate, channels: 2) else { return }
    let node = AVAudioSourceNode(format: fmt) { _, _, frameCount, abl -> OSStatus in
      let list = UnsafeMutableAudioBufferListPointer(abl)
      guard list.count >= 2, let l = list[0].mData, let r = list[1].mData else { return noErr }
      ao_render(l.assumingMemoryBound(to: Float.self), r.assumingMemoryBound(to: Float.self), Int32(frameCount))
      return noErr
    }
    sourceNode = node
    engine.attach(node)
    engine.connect(node, to: engine.mainMixerNode, format: fmt)
    do { try engine.start() } catch { NSLog("Audio engine failed: \(error)") }
  }

  // MARK: battery save
  private var sramURL: URL? {
    guard let rom = romURL else { return nil }
    return Emulator.supportDir.appendingPathComponent("Saves")
      .appendingPathComponent(rom.deletingPathExtension().lastPathComponent + ".srm")
  }

  private func sramData() -> Data? {
    guard let s = snes, s.pointee.sram_size > 0 else { return nil }
    return Data(bytes: snes_sram(s), count: Int(s.pointee.sram_size))
  }

  private func loadSram() {
    guard let s = snes, let url = sramURL, let d = try? Data(contentsOf: url) else { lastSram = sramData() ?? Data(); return }
    let n = min(d.count, Int(s.pointee.sram_size))
    d.prefix(n).copyBytes(to: snes_sram(s), count: n)
    lastSram = sramData() ?? Data()
  }

  func flushSram() {
    guard let url = sramURL, let d = sramData(), d != lastSram else { return }
    try? d.write(to: url, options: .atomic)
    lastSram = d
  }

  // MARK: save states
  private static let stateMagic = Data("SMWRSTAT".utf8)

  func stateURL(slot: Int) -> URL {
    Emulator.supportDir.appendingPathComponent("States").appendingPathComponent("\(romID)-slot\(slot).state")
  }

  func slotDate(_ slot: Int) -> Date? {
    (try? FileManager.default.attributesOfItem(atPath: stateURL(slot: slot).path))?[.modificationDate] as? Date
  }

  func saveState(to url: URL) -> Bool {
    guard let s = snes else { return false }
    let size = snes_state_size()
    var buf = Data(count: size)
    let ok = buf.withUnsafeMutableBytes { snes_save_state(s, $0.baseAddress, size) }
    guard ok else { return false }
    var out = Emulator.stateMagic
    out.append(Data(romID.utf8.prefix(12)))
    out.append(buf)
    do { try out.write(to: url, options: .atomic); return true } catch { return false }
  }

  func loadState(from url: URL) -> Bool {
    guard let s = snes, let d = try? Data(contentsOf: url), d.count > 20, d.prefix(8) == Emulator.stateMagic else { return false }
    let fileRom = String(decoding: d.subdata(in: 8..<20), as: UTF8.self)
    guard fileRom == romID else { onMessage?("State belongs to a different ROM"); return false }
    let body = d.subdata(in: 20..<d.count)
    let ok = body.withUnsafeBytes { snes_load_state(s, $0.baseAddress, body.count) }
    if ok { ao_clear() }
    return ok
  }

  func saveSlot(_ slot: Int) {
    onMessage?(saveState(to: stateURL(slot: slot)) ? "Saved state to slot \(slot)" : "Save failed")
  }

  func loadSlot(_ slot: Int) {
    if slotDate(slot) == nil { onMessage?("Slot \(slot) is empty"); return }
    onMessage?(loadState(from: stateURL(slot: slot)) ? "Loaded slot \(slot)" : "Load failed")
  }

  deinit { flushSram() }
}
