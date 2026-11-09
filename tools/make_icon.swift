// Renders the app icon (original artwork) into an .iconset directory.
// usage: swift make_icon.swift <out.iconset>
import Foundation
import CoreGraphics
import CoreText
import ImageIO
import UniformTypeIdentifiers

func rgb(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> CGColor {
  CGColor(srgbRed: r / 255, green: g / 255, blue: b / 255, alpha: a)
}

func gradient(_ ctx: CGContext, _ colors: [CGColor], _ locs: [CGFloat], from: CGPoint, to: CGPoint) {
  let g = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: colors as CFArray, locations: locs)!
  ctx.drawLinearGradient(g, start: from, end: to, options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
}

func radial(_ ctx: CGContext, _ colors: [CGColor], _ locs: [CGFloat], center: CGPoint, radius: CGFloat) {
  let g = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: colors as CFArray, locations: locs)!
  ctx.drawRadialGradient(g, startCenter: center, startRadius: 0, endCenter: center, endRadius: radius, options: [])
}

func star(center c: CGPoint, outer: CGFloat, inner: CGFloat, points: Int, rotation: CGFloat = 0) -> CGPath {
  let p = CGMutablePath()
  for i in 0..<(points * 2) {
    let r = i % 2 == 0 ? outer : inner
    let a = rotation + CGFloat(i) * .pi / CGFloat(points) + .pi / 2
    let pt = CGPoint(x: c.x + cos(a) * r, y: c.y + sin(a) * r)
    if i == 0 { p.move(to: pt) } else { p.addLine(to: pt) }
  }
  p.closeSubpath()
  return p
}

func cloud(_ ctx: CGContext, x: CGFloat, y: CGFloat, s: CGFloat) {
  let p = CGMutablePath()
  p.addEllipse(in: CGRect(x: x, y: y, width: 120 * s, height: 70 * s))
  p.addEllipse(in: CGRect(x: x + 50 * s, y: y + 25 * s, width: 110 * s, height: 85 * s))
  p.addEllipse(in: CGRect(x: x + 120 * s, y: y, width: 110 * s, height: 70 * s))
  ctx.saveGState()
  ctx.setShadow(offset: CGSize(width: 0, height: -6 * s), blur: 14 * s, color: rgb(40, 90, 160, 0.25))
  ctx.addPath(p); ctx.setFillColor(rgb(255, 255, 255)); ctx.fillPath()
  ctx.restoreGState()
}

func drawIcon(_ ctx: CGContext) {
  // macOS icon grid: 824pt body inside a 1024 canvas
  let body = CGRect(x: 100, y: 100, width: 824, height: 824)
  let shape = CGPath(roundedRect: body, cornerWidth: 185, cornerHeight: 185, transform: nil)

  // drop shadow
  ctx.saveGState()
  ctx.setShadow(offset: CGSize(width: 0, height: -14), blur: 28, color: rgb(0, 0, 0, 0.35))
  ctx.addPath(shape); ctx.setFillColor(rgb(60, 140, 230)); ctx.fillPath()
  ctx.restoreGState()

  ctx.saveGState()
  ctx.addPath(shape); ctx.clip()

  // sky
  gradient(ctx, [rgb(46, 120, 230), rgb(92, 178, 255), rgb(170, 225, 255)], [0, 0.55, 1],
           from: CGPoint(x: 512, y: 924), to: CGPoint(x: 512, y: 330))
  // sun glow
  radial(ctx, [rgb(255, 250, 210, 0.85), rgb(255, 250, 210, 0)], [0, 1], center: CGPoint(x: 512, y: 640), radius: 380)

  cloud(ctx, x: 150, y: 760, s: 0.9)
  cloud(ctx, x: 640, y: 800, s: 0.75)

  // back hills
  let back = CGMutablePath()
  back.move(to: CGPoint(x: 100, y: 330))
  back.addCurve(to: CGPoint(x: 470, y: 400), control1: CGPoint(x: 200, y: 520), control2: CGPoint(x: 380, y: 540))
  back.addCurve(to: CGPoint(x: 924, y: 380), control1: CGPoint(x: 600, y: 600), control2: CGPoint(x: 840, y: 560))
  back.addLine(to: CGPoint(x: 924, y: 100)); back.addLine(to: CGPoint(x: 100, y: 100)); back.closeSubpath()
  ctx.saveGState(); ctx.addPath(back); ctx.clip()
  gradient(ctx, [rgb(120, 205, 120), rgb(60, 160, 90)], [0, 1], from: CGPoint(x: 512, y: 560), to: CGPoint(x: 512, y: 300))
  ctx.restoreGState()
  ctx.addPath(back); ctx.setStrokeColor(rgb(25, 90, 50)); ctx.setLineWidth(10); ctx.strokePath()

  // front hill
  let front = CGMutablePath()
  front.move(to: CGPoint(x: 100, y: 250))
  front.addCurve(to: CGPoint(x: 924, y: 270), control1: CGPoint(x: 330, y: 420), control2: CGPoint(x: 700, y: 430))
  front.addLine(to: CGPoint(x: 924, y: 100)); front.addLine(to: CGPoint(x: 100, y: 100)); front.closeSubpath()
  ctx.saveGState(); ctx.addPath(front); ctx.clip()
  gradient(ctx, [rgb(90, 200, 70), rgb(40, 140, 50)], [0, 1], from: CGPoint(x: 512, y: 400), to: CGPoint(x: 512, y: 150))
  // spots
  ctx.setFillColor(rgb(30, 110, 40, 0.55))
  for (x, y, r) in [(260.0, 300.0, 16.0), (330.0, 250.0, 10.0), (700.0, 320.0, 14.0), (760.0, 260.0, 9.0), (520.0, 280.0, 11.0)] {
    ctx.fillEllipse(in: CGRect(x: x - r, y: y - r * 1.4, width: r * 2, height: r * 2.8))
  }
  ctx.restoreGState()
  ctx.addPath(front); ctx.setStrokeColor(rgb(20, 80, 30)); ctx.setLineWidth(10); ctx.strokePath()

  // ground strip
  ctx.setFillColor(rgb(214, 150, 82)); ctx.fill(CGRect(x: 100, y: 100, width: 824, height: 70))
  ctx.setFillColor(rgb(176, 110, 56))
  for i in 0..<14 { ctx.fill(CGRect(x: 110 + CGFloat(i) * 62, y: 118, width: 22, height: 12)) }
  ctx.setFillColor(rgb(250, 230, 160)); ctx.fill(CGRect(x: 100, y: 164, width: 824, height: 8))

  // the coin
  let cc = CGPoint(x: 512, y: 560)
  ctx.saveGState()
  ctx.setShadow(offset: CGSize(width: 0, height: -18), blur: 30, color: rgb(80, 50, 0, 0.45))
  ctx.setFillColor(rgb(196, 120, 10))
  ctx.fillEllipse(in: CGRect(x: cc.x - 215, y: cc.y - 250, width: 430, height: 500))
  ctx.restoreGState()
  ctx.saveGState()
  ctx.addEllipse(in: CGRect(x: cc.x - 200, y: cc.y - 235, width: 400, height: 470)); ctx.clip()
  gradient(ctx, [rgb(255, 244, 150), rgb(255, 205, 40), rgb(232, 150, 10)], [0, 0.5, 1],
           from: CGPoint(x: cc.x - 150, y: cc.y + 220), to: CGPoint(x: cc.x + 150, y: cc.y - 220))
  ctx.restoreGState()
  // inner rim
  ctx.setStrokeColor(rgb(214, 135, 10)); ctx.setLineWidth(16)
  ctx.strokeEllipse(in: CGRect(x: cc.x - 150, y: cc.y - 180, width: 300, height: 360))
  ctx.setStrokeColor(rgb(255, 250, 200, 0.8)); ctx.setLineWidth(6)
  ctx.strokeEllipse(in: CGRect(x: cc.x - 158, y: cc.y - 188, width: 316, height: 376))

  // embossed "W" (for World) in rounded bold type
  let font = CTFontCreateWithName("SFProRounded-Black" as CFString, 270, nil)
  let fallback = CTFontCreateUIFontForLanguage(.system, 270, nil)!
  let useFont = CTFontCopyPostScriptName(font) as String == "SFProRounded-Black" ? font :
    (CTFontCreateCopyWithSymbolicTraits(fallback, 270, nil, .traitBold, .traitBold) ?? fallback)
  func drawW(color: CGColor, dx: CGFloat, dy: CGFloat) {
    let attrs: [NSAttributedString.Key: Any] = [.init(kCTFontAttributeName as String): useFont,
                                                .init(kCTForegroundColorAttributeName as String): color]
    let line = CTLineCreateWithAttributedString(NSAttributedString(string: "W", attributes: attrs))
    let b = CTLineGetBoundsWithOptions(line, .useGlyphPathBounds)
    ctx.textPosition = CGPoint(x: cc.x - b.midX + dx, y: cc.y - b.midY + dy)
    CTLineDraw(line, ctx)
  }
  drawW(color: rgb(255, 250, 210), dx: -5, dy: 5)
  drawW(color: rgb(200, 120, 5), dx: 4, dy: -6)
  drawW(color: rgb(240, 165, 20), dx: 0, dy: 0)

  // specular highlight
  ctx.saveGState()
  let hl = CGMutablePath()
  hl.addEllipse(in: CGRect(x: cc.x - 165, y: cc.y + 40, width: 150, height: 170))
  ctx.addPath(hl); ctx.clip()
  radial(ctx, [rgb(255, 255, 255, 0.75), rgb(255, 255, 255, 0)], [0, 1], center: CGPoint(x: cc.x - 100, y: cc.y + 140), radius: 110)
  ctx.restoreGState()

  // sparkles
  for (x, y, r) in [(745.0, 735.0, 46.0), (285.0, 430.0, 30.0), (770.0, 470.0, 22.0)] {
    ctx.saveGState()
    ctx.setShadow(offset: .zero, blur: 18, color: rgb(255, 255, 220, 0.9))
    ctx.addPath(star(center: CGPoint(x: x, y: y), outer: r, inner: r * 0.22, points: 4))
    ctx.setFillColor(rgb(255, 255, 245)); ctx.fillPath()
    ctx.restoreGState()
  }

  // glassy top sheen
  ctx.saveGState()
  ctx.addRect(CGRect(x: 100, y: 600, width: 824, height: 324)); ctx.clip()
  gradient(ctx, [rgb(255, 255, 255, 0.18), rgb(255, 255, 255, 0)], [0, 1], from: CGPoint(x: 512, y: 924), to: CGPoint(x: 512, y: 600))
  ctx.restoreGState()
  ctx.restoreGState()

  // edge stroke
  ctx.addPath(shape); ctx.setStrokeColor(rgb(0, 0, 0, 0.18)); ctx.setLineWidth(3); ctx.strokePath()
}

let out = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "AppIcon.iconset")
try? FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
let master = 1024
let cs = CGColorSpace(name: CGColorSpace.sRGB)!
let big = CGContext(data: nil, width: master, height: master, bitsPerComponent: 8, bytesPerRow: 0, space: cs,
                    bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
big.interpolationQuality = .high
drawIcon(big)
let image = big.makeImage()!

func write(_ img: CGImage, _ name: String) {
  let url = out.appendingPathComponent(name)
  let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)!
  CGImageDestinationAddImage(dest, img, nil)
  CGImageDestinationFinalize(dest)
}

for (size, scales) in [(16, [1, 2]), (32, [1, 2]), (128, [1, 2]), (256, [1, 2]), (512, [1, 2])] {
  for sc in scales {
    let px = size * sc
    let c = CGContext(data: nil, width: px, height: px, bitsPerComponent: 8, bytesPerRow: 0, space: cs,
                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    c.interpolationQuality = .high
    c.draw(image, in: CGRect(x: 0, y: 0, width: px, height: px))
    write(c.makeImage()!, sc == 1 ? "icon_\(size)x\(size).png" : "icon_\(size)x\(size)@2x.png")
  }
}
do {
  let url = out.deletingLastPathComponent().appendingPathComponent("AppIcon-1024.png")
  let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)!
  CGImageDestinationAddImage(dest, image, nil); CGImageDestinationFinalize(dest)
}
print("icon written to \(out.path)")
