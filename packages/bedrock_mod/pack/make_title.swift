// Draws the title screen's wordmark in place of the Minecraft logo, white
// Poppins like OneClient's. Run: swift make_title.swift <font.ttf> <out.png>
import AppKit
import CoreText

let args = CommandLine.arguments
let (w, h) = (1937, 333)
CTFontManagerRegisterFontsForURL(URL(fileURLWithPath: args[1]) as CFURL, .process, nil)
let font = NSFont(name: "Poppins-SemiBold", size: 230)!
let text = NSAttributedString(string: "ONECLIENT", attributes: [
    .font: font, .foregroundColor: NSColor.white, .kern: 18,
])
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: w, pixelsHigh: h, bitsPerSample: 8,
                           samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                           bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
let size = text.size()
let shadow = NSShadow()
shadow.shadowColor = NSColor(white: 0, alpha: 0.45)
shadow.shadowBlurRadius = 18
shadow.set()
text.draw(at: NSPoint(x: (CGFloat(w) - size.width) / 2, y: (CGFloat(h) - size.height) / 2))
NSGraphicsContext.current = nil
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: args[2]))
