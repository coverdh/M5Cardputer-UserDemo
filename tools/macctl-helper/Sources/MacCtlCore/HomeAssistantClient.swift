import Foundation

public struct HomeAssistantConfig: Equatable {
    public var baseURL: URL
    public var token: String
    public var entityID: String

    public init(baseURL: URL, token: String, entityID: String) {
        self.baseURL = baseURL
        self.token = token
        self.entityID = entityID
    }
}

public struct HomeAssistantState: Decodable, Equatable {
    public struct Attributes: Decodable, Equatable {
        public var volumeLevel: Double?
        public var isVolumeMuted: Bool?
        public var mediaTitle: String?
        public var mediaArtist: String?
        public var mediaAlbumName: String?
        public var mediaPosition: Double?
        public var mediaDuration: Double?

        enum CodingKeys: String, CodingKey {
            case volumeLevel = "volume_level"
            case isVolumeMuted = "is_volume_muted"
            case mediaTitle = "media_title"
            case mediaArtist = "media_artist"
            case mediaAlbumName = "media_album_name"
            case mediaPosition = "media_position"
            case mediaDuration = "media_duration"
        }
    }

    public var state: String
    public var attributes: Attributes
}

public final class HomeAssistantClient {
    private let config: HomeAssistantConfig
    private let session: URLSession
    private let muteCacheURL: URL

    public init(config: HomeAssistantConfig, session: URLSession = .shared) {
        self.config = config
        self.session = session
        self.muteCacheURL = URL(fileURLWithPath: NSHomeDirectory())
            .appendingPathComponent(".config/karabiner/.ha_appletv_last_volume")
    }

    public func handle(_ command: MacCtlCommand) async throws {
        switch command {
        case .volumeDelta(let delta):
            try await applyVolumeDelta(delta)
        case .playPause:
            try await toggleMute()
        }
    }

    public func fetchState() async throws -> HomeAssistantState {
        let data = try await request("GET", path: "/api/states/\(config.entityID)")
        return try JSONDecoder().decode(HomeAssistantState.self, from: data)
    }

    private func applyVolumeDelta(_ delta: Int) async throws {
        guard delta != 0 else {
            return
        }

        let service = delta > 0 ? "volume_up" : "volume_down"
        for _ in 0..<min(abs(delta), 5) {
            try await callService("media_player", service, body: ["entity_id": config.entityID])
        }
    }

    private func toggleMute() async throws {
        let state = try await fetchState()
        let currentVolume = state.attributes.volumeLevel ?? 1.0
        let muted = state.attributes.isVolumeMuted ?? (currentVolume <= 0.001)
        do {
            try await callService("media_player", "volume_mute", body: ["entity_id": config.entityID, "is_volume_muted": !muted])
        } catch {
            try await fallbackMuteToggle(targetMute: !muted, currentVolume: currentVolume)
        }
    }

    private func fallbackMuteToggle(targetMute: Bool, currentVolume: Double) async throws {
        if targetMute {
            if currentVolume > 0.001 {
                try? "\(currentVolume)\n".write(to: muteCacheURL, atomically: true, encoding: .utf8)
            }
            try await callService("media_player", "volume_set", body: ["entity_id": config.entityID, "volume_level": 0.0])
            return
        }

        var restoreVolume = 0.15
        if let cached = try? String(contentsOf: muteCacheURL).trimmingCharacters(in: .whitespacesAndNewlines),
           let value = Double(cached), value > 0.001 {
            restoreVolume = value
        }
        try await callService("media_player", "volume_set", body: ["entity_id": config.entityID, "volume_level": restoreVolume])
    }

    private func callService(_ domain: String, _ service: String, body: [String: Any]) async throws {
        let bodyData = try JSONSerialization.data(withJSONObject: body, options: [])
        _ = try await request("POST", path: "/api/services/\(domain)/\(service)", body: bodyData)
    }

    private func request(_ method: String, path: String, body: Data? = nil) async throws -> Data {
        let base = config.baseURL.absoluteString.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        guard let url = URL(string: base + path) else {
            throw URLError(.badURL)
        }
        var request = URLRequest(url: url)
        request.httpMethod = method
        request.setValue("Bearer \(config.token)", forHTTPHeaderField: "Authorization")
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = body

        let (data, response) = try await session.data(for: request)
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
            throw URLError(.badServerResponse)
        }
        return data
    }
}
