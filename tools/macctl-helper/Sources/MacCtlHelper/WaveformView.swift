import AppKit

final class WaveformView: NSView {
    var isActive = false {
        didSet {
            if isActive {
                phase = 0
                startTimer()
            } else {
                stopTimer()
            }
            needsDisplay = true
        }
    }

    private var phase: Double = 0
    private var timer: Timer?

    deinit {
        stopTimer()
    }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.cornerRadius = 8
        layer?.backgroundColor = NSColor.controlBackgroundColor.withAlphaComponent(0.72).cgColor
    }

    required init?(coder: NSCoder) {
        nil
    }

    private func startTimer() {
        guard timer == nil else { return }
        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.phase += 0.18
            self.needsDisplay = true
        }
    }

    private func stopTimer() {
        timer?.invalidate()
        timer = nil
    }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)

        let rect = bounds.insetBy(dx: 16, dy: 14)
        guard rect.width > 1, rect.height > 1 else { return }

        NSColor.separatorColor.withAlphaComponent(0.35).setStroke()
        let mid = rect.midY
        let centerLine = NSBezierPath()
        centerLine.move(to: NSPoint(x: rect.minX, y: mid))
        centerLine.line(to: NSPoint(x: rect.maxX, y: mid))
        centerLine.lineWidth = 1
        centerLine.stroke()

        let path = NSBezierPath()
        let samples = max(64, Int(rect.width / 3))
        for index in 0...samples {
            let x = rect.minX + CGFloat(index) / CGFloat(samples) * rect.width
            let t = Double(index) / Double(samples)
            let envelope = isActive ? (0.35 + 0.45 * abs(sin(phase * 0.7 + t * 5.0))) : 0.05
            let wave = sin(t * 38.0 + phase) * 0.55 + sin(t * 77.0 + phase * 1.6) * 0.25
            let y = mid + CGFloat(wave * envelope) * rect.height * 0.46
            if index == 0 {
                path.move(to: NSPoint(x: x, y: y))
            } else {
                path.line(to: NSPoint(x: x, y: y))
            }
        }
        NSColor.systemBlue.setStroke()
        path.lineWidth = 2.2
        path.stroke()

        if !isActive {
            let label = "Idle"
            let attrs: [NSAttributedString.Key: Any] = [
                .font: NSFont.systemFont(ofSize: 12, weight: .medium),
                .foregroundColor: NSColor.secondaryLabelColor,
            ]
            let size = label.size(withAttributes: attrs)
            label.draw(at: NSPoint(x: rect.midX - size.width / 2, y: rect.midY - size.height / 2), withAttributes: attrs)
        }
    }
}
