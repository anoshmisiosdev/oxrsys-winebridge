import Foundation

@MainActor
final class LibraryModel: ObservableObject {
    @Published var games: [Game] = []
    @Published var scanning = false
    @Published var showHidden = false
    @Published private(set) var prefs = LibraryPrefs.load()

    var visibleGames: [Game] {
        let hidden = Set(prefs.hiddenIDs)
        return games.filter { showHidden || !hidden.contains($0.id) }
    }

    func isHidden(_ g: Game) -> Bool { prefs.hiddenIDs.contains(g.id) }

    func rescan() {
        scanning = true
        Task.detached(priority: .userInitiated) {
            let scanned = LibraryScanner.scan()
            await MainActor.run {
                var all = scanned
                for m in self.prefs.manualGames where !all.contains(where: { $0.id == m.id }) {
                    all.append(m)
                }
                self.games = all
                self.scanning = false
            }
        }
    }

    func add(_ g: Game) {
        prefs.manualGames.removeAll { $0.id == g.id }
        prefs.manualGames.append(g)
        prefs.save()
        if let i = games.firstIndex(where: { $0.id == g.id }) { games[i] = g } else { games.append(g) }
    }

    func remove(_ g: Game) {
        prefs.manualGames.removeAll { $0.id == g.id }
        prefs.save()
        if g.manual { games.removeAll { $0.id == g.id } }
    }

    func toggleHidden(_ g: Game) {
        if let i = prefs.hiddenIDs.firstIndex(of: g.id) { prefs.hiddenIDs.remove(at: i) }
        else { prefs.hiddenIDs.append(g.id) }
        prefs.save()
    }
}
