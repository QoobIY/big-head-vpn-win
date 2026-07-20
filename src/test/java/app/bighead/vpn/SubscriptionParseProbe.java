package app.bighead.vpn;

import java.nio.file.*;
import java.util.*;
import java.util.regex.Pattern;

public final class SubscriptionParseProbe {
    public static void main(String[] args) throws Exception {
        var body=Files.readString(Path.of(args[0]));
        var matcher=Pattern.compile("(?i)(?:vless|hysteria2?|hy2)://[^\\s<>\\\"']+").matcher(body);
        int found=0,accepted=0;var errors=new TreeMap<String,Integer>();
        while(matcher.find()){found++;try{ProfileParser.outbound(ProfileParser.parse(matcher.group()));accepted++;}catch(Exception e){errors.merge(e.getClass().getSimpleName()+": "+e.getMessage(),1,Integer::sum);}}
        System.out.println("found="+found+" accepted="+accepted+" rejected="+(found-accepted));
        errors.forEach((message,count)->System.out.println(count+" x "+message));
    }
}
