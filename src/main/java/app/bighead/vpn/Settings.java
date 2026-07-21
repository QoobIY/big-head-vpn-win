package app.bighead.vpn;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

public final class Settings {
    public static final String MODE_TUN = "tun";
    public static final String MODE_PROXY = "proxy";
    public static final Path DIR = Path.of(
        System.getenv().getOrDefault("LOCALAPPDATA", System.getProperty("user.home")),
        "BigHeadVpn"
    );
    private static final Path FILE = DIR.resolve("settings.properties");

    public final List<Profile> profiles = new ArrayList<>();
    public final List<SubscriptionGroup> groups = new ArrayList<>();
    public String selectedId = "";
    public String listenAddress = "127.0.0.1";
    public int listenPort = 2080;
    public boolean listenerEnabled;
    public String mode = MODE_TUN;

    public static Settings load() {
        var settings = new Settings();
        var properties = new Properties();
        try (var input = Files.newInputStream(FILE)) {
            properties.loadFromXML(input);
            settings.selectedId = properties.getProperty("selected", "");
            settings.listenAddress = properties.getProperty("listenAddress", "127.0.0.1").trim();
            settings.listenPort = validPort(properties.getProperty("listenPort", "2080"));
            settings.listenerEnabled = Boolean.parseBoolean(properties.getProperty("listenerEnabled", "false"));
            settings.mode = MODE_PROXY.equals(properties.getProperty("mode")) ? MODE_PROXY : MODE_TUN;

            for (int i = 0; i < count(properties, "profiles"); i++) {
                var uri = properties.getProperty("p." + i + ".uri", "");
                if (uri.isBlank()) continue;
                settings.profiles.add(new Profile(
                    properties.getProperty("p." + i + ".id", UUID.randomUUID().toString()),
                    properties.getProperty("p." + i + ".name", "Сервер"),
                    uri,
                    properties.getProperty("p." + i + ".group", "")
                ));
            }
            for (int i = 0; i < count(properties, "groups"); i++) {
                var url = properties.getProperty("g." + i + ".url", "");
                if (url.isBlank()) continue;
                settings.groups.add(new SubscriptionGroup(
                    properties.getProperty("g." + i + ".id", UUID.randomUUID().toString()),
                    properties.getProperty("g." + i + ".name", "Подписка"),
                    url
                ));
            }
        } catch (NoSuchFileException ignored) {
            // First launch.
        } catch (Exception ignored) {
            // Keep safe defaults if an old or partially written settings file is corrupt.
        }
        return settings;
    }

    public synchronized void save() {
        try {
            Files.createDirectories(DIR);
            var properties = new Properties();
            properties.setProperty("selected", selectedId == null ? "" : selectedId);
            properties.setProperty("listenAddress", listenAddress);
            properties.setProperty("listenPort", Integer.toString(listenPort));
            properties.setProperty("listenerEnabled", Boolean.toString(listenerEnabled));
            properties.setProperty("mode", MODE_PROXY.equals(mode) ? MODE_PROXY : MODE_TUN);

            properties.setProperty("profiles", Integer.toString(profiles.size()));
            for (int i = 0; i < profiles.size(); i++) {
                var profile = profiles.get(i);
                properties.setProperty("p." + i + ".id", profile.id());
                properties.setProperty("p." + i + ".name", profile.name());
                properties.setProperty("p." + i + ".uri", profile.uri());
                properties.setProperty("p." + i + ".group", profile.groupId());
            }
            properties.setProperty("groups", Integer.toString(groups.size()));
            for (int i = 0; i < groups.size(); i++) {
                var group = groups.get(i);
                properties.setProperty("g." + i + ".id", group.id());
                properties.setProperty("g." + i + ".name", group.name());
                properties.setProperty("g." + i + ".url", group.url());
            }

            var temporary = DIR.resolve("settings.properties.tmp");
            try (var output = Files.newOutputStream(temporary, StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING)) {
                properties.storeToXML(output, "Big Head VPN settings", StandardCharsets.UTF_8);
            }
            try {
                Files.move(temporary, FILE, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            } catch (AtomicMoveNotSupportedException ignored) {
                Files.move(temporary, FILE, StandardCopyOption.REPLACE_EXISTING);
            }
        } catch (IOException error) {
            throw new UncheckedIOException("Не удалось сохранить настройки", error);
        }
    }

    private static int count(Properties properties, String key) {
        try { return Math.max(0, Integer.parseInt(properties.getProperty(key, "0"))); }
        catch (NumberFormatException ignored) { return 0; }
    }

    private static int validPort(String value) {
        try {
            int port = Integer.parseInt(value);
            return port >= 1 && port <= 65535 ? port : 2080;
        } catch (NumberFormatException ignored) {
            return 2080;
        }
    }
}
