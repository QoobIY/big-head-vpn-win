package app.bighead.vpn;

import java.util.UUID;

public record Profile(String id, String name, String uri, String groupId) {
    public Profile(String name, String uri) { this(UUID.randomUUID().toString(), name, uri, ""); }
    public Profile(String id, String name, String uri) { this(id, name, uri, ""); }
    public Profile inGroup(String value) { return new Profile(id, name, uri, value); }
    @Override public String toString() { return name; }
}
