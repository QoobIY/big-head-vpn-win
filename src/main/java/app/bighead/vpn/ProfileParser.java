package app.bighead.vpn;

import java.net.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

public final class ProfileParser {
    private ProfileParser() {}
    public static Profile parse(String text) {
        try {
            text=text.trim(); var uri = connectionUri(text); var scheme = lower(uri.getScheme());
            if (!Set.of("vless","hysteria","hysteria2","hy2").contains(scheme)) throw new IllegalArgumentException();
            if (uri.getHost()==null || uri.getPort()<1) throw new IllegalArgumentException("В ссылке нет корректного сервера или порта");
            var hash=text.indexOf('#');var name=decode(hash<0?"":text.substring(hash+1));return new Profile(name.isBlank()?uri.getHost():name,text);
        } catch (IllegalArgumentException e) {
            if (e.getMessage()!=null && e.getMessage().startsWith("В ссылке")) throw e;
            throw new IllegalArgumentException("Поддерживаются vless://, hysteria://, hysteria2:// и hy2://");
        }
    }
    public static String outbound(Profile profile) { return outbound(profile,""); }
    public static String outbound(Profile profile,String bindInterface) {
        var u=connectionUri(profile.uri()); var q=query(u); var scheme=lower(u.getScheme());
        if (scheme.equals("vless")) {
            String tls=null, transport=null, security=q.getOrDefault("security","");
            if (security.equals("tls") || security.equals("reality")) tls=tls(q,u.getHost(),security.equals("reality"));
            var type=q.getOrDefault("type","tcp");
            if (!type.equals("tcp") && !type.equals("raw")) transport=transport(type,q);
            return Json.object("type","vless","tag","proxy","server",u.getHost(),"server_port",u.getPort(),"uuid",decode(u.getRawUserInfo()),"bind_interface",blankToNull(bindInterface),
                    "flow",blankToNull(q.get("flow")),"tls",tls==null?null:Json.raw(tls),"transport",transport==null?null:Json.raw(transport));
        }
        String obfs=null;
        if (!q.getOrDefault("obfs","").isBlank()) obfs=Json.object("type",q.get("obfs"),"password",q.getOrDefault("obfs-password",q.getOrDefault("obfs_password","")));
        return Json.object("type","hysteria2","tag","proxy","server",u.getHost(),"server_port",u.getPort(),"password",decode(u.getRawUserInfo()),"bind_interface",blankToNull(bindInterface),
                "tls",Json.raw(tls(q,u.getHost(),false)),"obfs",obfs==null?null:Json.raw(obfs));
    }
    static String serverHost(Profile profile){return connectionUri(profile.uri()).getHost();}
    private static String tls(Map<String,String> q,String host,boolean reality) {
        String r=null; if (reality) r=Json.object("enabled",true,"public_key",q.getOrDefault("pbk",""),"short_id",q.getOrDefault("sid",""));
        String utls=null;if(reality)utls=Json.object("enabled",true,"fingerprint",q.getOrDefault("fp","chrome"));
        return Json.object("enabled",true,"server_name",q.getOrDefault("sni",host),"insecure",q.getOrDefault("insecure",q.getOrDefault("allowInsecure","0")).equals("1"),"utls",utls==null?null:Json.raw(utls),"reality",r==null?null:Json.raw(r));
    }
    private static String transport(String type,Map<String,String> q) { return switch(type) {
        case "ws" -> Json.object("type","ws","path",q.getOrDefault("path","/"),"headers",Json.raw(Json.object("Host",q.getOrDefault("host",""))));
        case "grpc" -> Json.object("type","grpc","service_name",q.getOrDefault("serviceName",""));
        case "http", "h2" -> Json.object("type","http","host",List.of(q.getOrDefault("host","")),"path",q.getOrDefault("path","/"));
        case "httpupgrade" -> Json.object("type","httpupgrade","path",q.getOrDefault("path","/"),"host",q.getOrDefault("host",""));
        default -> throw new IllegalArgumentException("Transport '"+type+"' пока не поддерживается");
    }; }
    private static Map<String,String> query(URI u) { var result=new TreeMap<String,String>(String.CASE_INSENSITIVE_ORDER); if(u.getRawQuery()==null)return result; for(var p:u.getRawQuery().split("&")){var x=p.split("=",2);result.put(decode(x[0]),x.length>1?decode(x[1]):"");} return result; }
    private static URI connectionUri(String text) { var clean=text.trim();var hash=clean.indexOf('#');if(hash>=0)clean=clean.substring(0,hash);return URI.create(clean); }
    private static String decode(String value) { return value==null?"":URLDecoder.decode(value.replace("+","%2B"),StandardCharsets.UTF_8); }
    private static String lower(String s){return s==null?"":s.toLowerCase(Locale.ROOT);} private static String blankToNull(String s){return s==null||s.isBlank()?null:s;}
}
