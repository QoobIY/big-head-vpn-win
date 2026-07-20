package app.bighead.vpn;

import java.util.UUID;

public record SubscriptionGroup(String id, String name, String url) {
    public SubscriptionGroup(String name, String url) { this(UUID.randomUUID().toString(), name, url); }
    @Override public String toString() { return name; }
}
