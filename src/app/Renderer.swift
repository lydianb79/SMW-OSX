import MetalKit

struct ShaderUniforms {
  var srcSize: SIMD2<Float>
  var outSize: SIMD2<Float>
  var ambientScale: SIMD2<Float>
  var time: Float
  var filter: Int32
  var bloom: Float
  var vibrance: Float
  var scanStyle: Int32
  var scanIntensity: Float
  var scanThickness: Float
  var maskType: Int32
  var maskStrength: Float
  var scanBoost: Float
}

final class Renderer: NSObject, MTKViewDelegate {
  let device: MTLDevice
  private let queue: MTLCommandQueue
  private var classicPipe: MTLRenderPipelineState!
  private var modernPipe: MTLRenderPipelineState!
  private var ambientPipe: MTLRenderPipelineState!
  private var texture: MTLTexture?
  private var texHeight = 0
  private let start = CACurrentMediaTime()
  weak var emulator: Emulator?

  init(view: MTKView) {
    device = view.device!
    queue = device.makeCommandQueue()!
    super.init()
    buildPipelines(format: view.colorPixelFormat)
  }

  private func loadLibrary() -> MTLLibrary {
    if let url = Bundle.main.url(forResource: "default", withExtension: "metallib"),
       let lib = try? device.makeLibrary(URL: url) { return lib }
    let srcURL = Bundle.main.url(forResource: "Shaders", withExtension: "metal")!
    let src = try! String(contentsOf: srcURL, encoding: .utf8)
    do { return try device.makeLibrary(source: src, options: nil) }
    catch { fatalError("Shader compile failed: \(error)") }
  }

  private func buildPipelines(format: MTLPixelFormat) {
    let lib = loadLibrary()
    func make(_ frag: String) -> MTLRenderPipelineState {
      let d = MTLRenderPipelineDescriptor()
      d.vertexFunction = lib.makeFunction(name: "vtx_main")
      d.fragmentFunction = lib.makeFunction(name: frag)
      d.colorAttachments[0].pixelFormat = format
      return try! device.makeRenderPipelineState(descriptor: d)
    }
    classicPipe = make("frag_classic")
    modernPipe = make("frag_modern")
    ambientPipe = make("frag_ambient")
  }

  func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

  private func ensureTexture(height: Int) -> MTLTexture {
    if let t = texture, texHeight == height { return t }
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: 256, height: height, mipmapped: true)
    d.usage = [.shaderRead]
    d.storageMode = .shared
    texture = device.makeTexture(descriptor: d)
    texHeight = height
    return texture!
  }

  /// Computes the game rectangle inside the drawable.
  private func gameRect(drawable: CGSize, srcH: Int) -> CGRect {
    let s = Settings.shared
    let W = drawable.width, H = drawable.height, sh = CGFloat(srcH)
    if s.aspect == .stretch { return CGRect(x: 0, y: 0, width: W, height: H) }
    let aspect: CGFloat = s.aspect == .tv43 ? 4.0 / 3.0 : 256.0 / sh
    var w = W, h = W / aspect
    if h > H { h = H; w = H * aspect }
    if s.integerScale {
      let k = max(1, floor(min(H / sh, W / (sh * aspect))))
      h = sh * k; w = h * aspect
    }
    return CGRect(x: floor((W - w) / 2), y: floor((H - h) / 2), width: floor(w), height: floor(h))
  }

  func draw(in view: MTKView) {
    emulator?.tick()
    guard let emu = emulator, let snes = emu.snes else {
      if let rpd = view.currentRenderPassDescriptor, let dr = view.currentDrawable,
         let cb = queue.makeCommandBuffer(), let enc = cb.makeRenderCommandEncoder(descriptor: rpd) {
        enc.endEncoding(); cb.present(dr); cb.commit()
      }
      return
    }
    let h = Int(snes.pointee.fb_height)
    let tex = ensureTexture(height: h)
    tex.replace(region: MTLRegionMake2D(0, 0, 256, h), mipmapLevel: 0,
                withBytes: UnsafeRawPointer(snes_framebuffer(snes)), bytesPerRow: 256 * 4)
    guard let cb = queue.makeCommandBuffer() else { return }
    let s = Settings.shared
    if s.modernGraphics, let blit = cb.makeBlitCommandEncoder() {
      blit.generateMipmaps(for: tex)
      blit.endEncoding()
    }
    guard let rpd = view.currentRenderPassDescriptor, let drawable = view.currentDrawable else { cb.commit(); return }
    rpd.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
    rpd.colorAttachments[0].loadAction = .clear
    guard let enc = cb.makeRenderCommandEncoder(descriptor: rpd) else { return }
    let ds = view.drawableSize
    let rect = gameRect(drawable: ds, srcH: h)
    var u = ShaderUniforms(
      srcSize: SIMD2(256, Float(h)), outSize: SIMD2(Float(rect.width), Float(rect.height)),
      ambientScale: SIMD2(1, 1), time: Float(CACurrentMediaTime() - start),
      filter: s.filter.index, bloom: Float(s.bloom), vibrance: Float(s.vibrance),
      scanStyle: s.scanStyle.index, scanIntensity: Float(s.scanIntensity), scanThickness: Float(s.scanThickness),
      maskType: s.maskType.index, maskStrength: Float(s.maskStrength), scanBoost: Float(s.scanBoost))
    enc.setFragmentTexture(tex, index: 0)

    if s.modernGraphics && s.ambientFill && (rect.width < ds.width - 1 || rect.height < ds.height - 1) {
      // cover-fit scale so the blurred backdrop fills the window
      let sx = Float(rect.width / ds.width), sy = Float(rect.height / ds.height)
      let m = max(sx, sy)
      u.ambientScale = SIMD2(sx / m, sy / m)
      enc.setViewport(MTLViewport(originX: 0, originY: 0, width: Double(ds.width), height: Double(ds.height), znear: 0, zfar: 1))
      enc.setRenderPipelineState(ambientPipe)
      enc.setFragmentBytes(&u, length: MemoryLayout<ShaderUniforms>.stride, index: 0)
      enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
    }
    enc.setViewport(MTLViewport(originX: Double(rect.minX), originY: Double(rect.minY),
                                width: Double(rect.width), height: Double(rect.height), znear: 0, zfar: 1))
    enc.setRenderPipelineState(s.modernGraphics ? modernPipe : classicPipe)
    enc.setFragmentBytes(&u, length: MemoryLayout<ShaderUniforms>.stride, index: 0)
    enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
    enc.endEncoding()
    cb.present(drawable)
    cb.commit()
  }
}
