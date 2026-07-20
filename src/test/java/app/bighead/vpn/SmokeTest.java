package app.bighead.vpn;

public final class SmokeTest {
    public static void main(String[] args) {
        var vless=ProfileParser.parse("vless://12345678-1234-1234-1234-123456789abc@example.com:443?security=reality&sni=cdn.example.com&pbk=key&sid=12&type=ws&path=%2Fvpn#Test%20server");
        require(vless.name().equals("Test server"),"VLESS profile name");
        var vlessJson=ProfileParser.outbound(vless);require(vlessJson.contains("\"type\":\"vless\"")&&vlessJson.contains("\"reality\""),"VLESS outbound: "+vlessJson);
        var hy2=ProfileParser.parse("hy2://secret@example.com:8443?sni=example.com&obfs=salamander&obfs-password=test#HY2");
        var settings=new Settings();settings.profiles.add(hy2);settings.processes.add("chrome.exe");settings.listenerEnabled=true;
        var config=SingBox.config(hy2,settings);require(config.contains("\"process_name\":[\"chrome.exe\"]"),"process rule");require(config.contains("\"listen\":\"0.0.0.0\""),"listener");
        try { ProfileParser.parse("ss://invalid"); throw new AssertionError("unsupported URI accepted"); } catch (IllegalArgumentException expected) {}
        System.out.println(config);
        System.out.println("Smoke tests passed");
    }
    private static void require(boolean value,String message){if(!value)throw new AssertionError(message);}
}
