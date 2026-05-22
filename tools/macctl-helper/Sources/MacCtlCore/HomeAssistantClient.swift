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

        enum CodingKeys: String, CodingKey {
            case volumeLevel = "volume_level"
        }
    }

    public var state: String
    public var attributes: Attributes
}

public final class HomeAssistantClient {
    private let config: HomeAssistantConfig
    private let session: URLSession

    public init(config: HomeAssistantConfig, session: URLSession = .shared) {
        self.config = config
        self.session = session
    }

    public func handle(_ command: MacCtlCommand) async throws {
        switch command {
        case .volumeDelta(let delta):
            try await applyVolumeDelta(delta)
        case .playPause:
            try await callService("media_player", "media_play_pause", body: ["entity_id": config.entityID])
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

        let state = try await fetchState()
        if let volumeLevel = state.attributes.volumeLevel {
            let current = Int((volumeLevel * 100.0).rounded())
            let target = targetVolumePercent(current: current, delta: delta)
            try await callService(
                "media_player",
                "volume_set",
                body: ["entity_id": config.entityID, "volume_level": Double(target) / 100.0]
            )
            return
        }

        let service = delta > 0 ? "volume_up" : "volume_down"
        for _ in 0..<min(abs(delta), 5) {
            try await callService("media_player", service, body: ["entity_id": config.entityID])
        }
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
