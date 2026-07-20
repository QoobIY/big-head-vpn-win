package app.bighead.vpn;

import java.net.URI;
import java.net.http.*;
import java.time.Duration;
import java.nio.charset.StandardCharsets;
import java.util.regex.Pattern;

public final class SubscriptionProbe {
    public static void main(String[] args) throws Exception {
        if (args.length != 1) throw new IllegalArgumentException("subscription URL required");
        var request=HttpRequest.newBuilder(URI.create(args[0])).timeout(Duration.ofSeconds(20)).header("User-Agent","BigHeadVPN/0.1").GET().build();
        var response=HttpClient.newBuilder().followRedirects(HttpClient.Redirect.NORMAL).connectTimeout(Duration.ofSeconds(12)).build().send(request,HttpResponse.BodyHandlers.ofByteArray());
        var body=new String(response.body(),StandardCharsets.UTF_8);
        var matcher=Pattern.compile("(?i)(?:vless|hysteria2?|hy2)://[^\\s<>\\\"']+").matcher(body);
        int count=0; while(matcher.find()) count++;
        System.out.println("status="+response.statusCode()+" bytes="+response.body().length+" profiles="+count);
    }
}
