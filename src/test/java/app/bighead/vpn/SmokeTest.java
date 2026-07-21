package app.bighead.vpn;

public final class SmokeTest {
    public static void main(String[] args) {
        var vless=ProfileParser.parse("vless://12345678-1234-1234-1234-123456789abc@example.com:443?security=reality&sni=cdn.example.com&pbk=key&sid=12&type=ws&path=%2Fvpn#Test%20server");
        require(vless.name().equals("Test server"),"VLESS profile name");
        var vlessJson=ProfileParser.outbound(vless);require(vlessJson.contains("\"type\":\"vless\"")&&vlessJson.contains("\"reality\""),"VLESS outbound: "+vlessJson);
        var hy2=ProfileParser.parse("hy2://secret@example.com:8443?sni=example.com&obfs=salamander&obfs-password=test#HY2");
        var settings=new Settings();settings.profiles.add(hy2);settings.listenerEnabled=true;
        var config=SingBox.config(hy2,settings);require(config.contains("\"type\":\"tun\""),"system mode TUN");require(!config.contains("process_name"),"process filtering removed");require(config.contains("\"mtu\":1400"),"TUN MTU");require(config.contains("\"final\":\"proxy\""),"all system traffic must use proxy");require(config.contains("\"listen\":\"127.0.0.1\""),"optional safe local listener");require(config.contains("\"inbound\":[\"local-proxy\"]"),"listener proxy rule");
        var bound=SingBox.config(hy2,settings,"Ethernet",java.util.List.of("203.0.113.8/32"));require(bound.contains("\"bind_interface\":\"Ethernet\""),"outbounds explicitly bound to physical interface");require(bound.contains("\"route_exclude_address\":[\"203.0.113.8/32\"]"),"VPN endpoint excluded from TUN route");
        var proxySettings=new Settings();proxySettings.mode=Settings.MODE_PROXY;proxySettings.listenerEnabled=false;var proxyConfig=SingBox.config(hy2,proxySettings);require(!proxyConfig.contains("\"type\":\"tun\""),"proxy server mode must not create TUN");require(proxyConfig.contains("\"type\":\"mixed\""),"proxy server mixed inbound");require(proxyConfig.contains("\"auto_detect_interface\":false"),"proxy server needs no TUN interface detection");
        try { ProfileParser.parse("ss://invalid"); throw new AssertionError("unsupported URI accepted"); } catch (IllegalArgumentException expected) {}
        require(SingBox.cleanLog("\u001b[31mОШИБКА\u001b[0m").equals("ОШИБКА"),"ANSI/UTF-8 log cleanup");
        System.out.println(config);
        System.out.println("Smoke tests passed");
    }
    private static void require(boolean value,String message){if(!value)throw new AssertionError(message);}
}
