import AppKit
import Foundation

struct Payload: Codable {
    let width: Int
    let height: Int
    let lines: [String]
}

func loadPayload() throws -> Payload {
    let data = FileHandle.standardInput.readDataToEndOfFile()
    return try JSONDecoder().decode(Payload.self, from: data)
}

func pickFont(size: CGFloat, bold: Bool) -> NSFont {
    let candidates = bold
        ? ["HiraginoSansGB-W6", "STHeitiSC-Medium", ".AppleSystemUIFont"]
        : ["HiraginoSansGB-W3", "STHeitiSC-Light", ".AppleSystemUIFont"]

    for name in candidates {
        if let font = NSFont(name: name, size: size) {
            return font
        }
    }
    return NSFont.systemFont(ofSize: size, weight: bold ? .semibold : .regular)
}

func averageBrightness(_ color: NSColor) -> CGFloat {
    let converted = color.usingColorSpace(.deviceRGB) ?? color
    return (converted.redComponent + converted.greenComponent + converted.blueComponent) / 3.0
}

let payload = try loadPayload()
let width = payload.width
let height = payload.height
let rowBytes = width / 8
let bitmapByteCount = rowBytes * height

let rep = NSBitmapImageRep(
    bitmapDataPlanes: nil,
    pixelsWide: width,
    pixelsHigh: height,
    bitsPerSample: 8,
    samplesPerPixel: 4,
    hasAlpha: true,
    isPlanar: false,
    colorSpaceName: .deviceRGB,
    bytesPerRow: 0,
    bitsPerPixel: 0
)!

NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)

NSColor.white.setFill()
NSBezierPath(rect: NSRect(x: 0, y: 0, width: width, height: height)).fill()

let bodyFont = pickFont(size: 16, bold: false)

let bodyAttrs: [NSAttributedString.Key: Any] = [
    .font: bodyFont,
    .foregroundColor: NSColor.black,
]

let margin: CGFloat = 8
let contentWidth = CGFloat(width) - margin * 2
var top: CGFloat = 12

for line in payload.lines.prefix(4) {
    let text = NSString(string: line)
    let rect = NSRect(
        x: margin,
        y: CGFloat(height) - top - 36,
        width: contentWidth,
        height: 36
    )
    text.draw(
        with: rect,
        options: [.usesLineFragmentOrigin, .usesFontLeading],
        attributes: bodyAttrs
    )
    top += 34
}

NSGraphicsContext.restoreGraphicsState()

var packed = [UInt8](repeating: 0xFF, count: bitmapByteCount)
for y in 0..<height {
    for x in 0..<width {
        let sourceY = height - 1 - y
        guard let color = rep.colorAt(x: x, y: sourceY) else {
            continue
        }
        if averageBrightness(color) < 0.85 {
            let index = y * rowBytes + (x / 8)
            let mask = UInt8(0x80 >> (x & 7))
            packed[index] &= ~mask
        }
    }
}

FileHandle.standardOutput.write(Data(packed))
