package app.bighead.vpn;

import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.function.Consumer;

public final class SingBox implements AutoCloseable {
    private volatile Process process; private final Consumer<String> log; private final Consumer<String> stopped; private volatile boolean stopping; private volatile String lastLine="";
    public SingBox(Consumer<String> log,Consumer<String> stopped){this.log=log;this.stopped=stopped;} public boolean running(){return process!=null&&process.isAlive();}
    public void start(Profile profile,Settings s)throws Exception{if(running())return;var exe=findExecutable();Files.createDirectories(Settings.DIR);var config=Settings.DIR.resolve("config.json");Files.writeString(config,config(profile,s));
        var check=new ProcessBuilder(exe.toString(),"check","-c",config.toString()).redirectErrorStream(true).start();var error=new String(check.getInputStream().readAllBytes());if(check.waitFor()!=0)throw new IOException("Ошибка конфигурации sing-box:\n"+error);
        stopping=false;lastLine="";var started=new ProcessBuilder(exe.toString(),"run","-c",config.toString()).redirectErrorStream(true).start();process=started;
        Thread.ofVirtual().start(()->{try(var r=started.inputReader()){r.lines().forEach(line->{lastLine=line;log.accept(line);});}catch(IOException ignored){}finally{if(process==started)process=null;if(!stopping)stopped.accept(lastLine.isBlank()?"Сетевое ядро неожиданно завершилось":lastLine);}});
        Thread.sleep(800);if(!started.isAlive())throw new IOException("sing-box не запустился: "+(lastLine.isBlank()?"неизвестная ошибка":lastLine));}
    public void stop(){var target=process;if(target==null)return;stopping=true;long pid=target.pid();target.descendants().forEach(h->{try{h.destroy();}catch(Exception ignored){}});target.destroy();try{if(!target.waitFor(1500,java.util.concurrent.TimeUnit.MILLISECONDS)){target.descendants().forEach(h->{try{h.destroyForcibly();}catch(Exception ignored){}});target.destroyForcibly();target.waitFor(1,java.util.concurrent.TimeUnit.SECONDS);}if(target.isAlive()&&System.getProperty("os.name","").startsWith("Windows")){new ProcessBuilder("taskkill","/PID",Long.toString(pid),"/T","/F").redirectErrorStream(true).start().waitFor();}}catch(Exception e){log.accept("Ошибка остановки: "+e.getMessage());}finally{if(process==target)process=null;stopping=false;stopped.accept("");}} public void close(){stop();}
    private static Path findExecutable()throws FileNotFoundException{for(var p:List.of(Path.of("tools","sing-box.exe"),Path.of(System.getProperty("java.home")).getParent().resolve("tools").resolve("sing-box.exe")))if(Files.exists(p))return p.toAbsolutePath();throw new FileNotFoundException("Не найден tools\\sing-box.exe. Запустите scripts\\download-sing-box.ps1");}
    static String config(Profile p,Settings s){var tun=Json.object("type","tun","tag","tun-in","interface_name","BigHeadVPN","address",List.of("172.19.0.1/30"),"auto_route",true,"strict_route",true,"stack","mixed");var ins=new ArrayList<String>();ins.add(tun);if(s.listenerEnabled)ins.add(Json.object("type","mixed","tag","local-proxy","listen",s.listenAddress,"listen_port",s.listenPort));
        var rules=new ArrayList<String>();if(!s.processes.isEmpty()){rules.add(Json.object("process_name",s.processes,"action","route","outbound","proxy"));rules.add(Json.object("action","route","outbound","direct"));}
        return Json.object("log",Json.raw(Json.object("level","info","timestamp",true)),"inbounds",Json.raw("["+String.join(",",ins)+"]"),"outbounds",Json.raw("["+ProfileParser.outbound(p)+","+Json.object("type","direct","tag","direct")+"]"),"route",Json.raw(Json.object("auto_detect_interface",true,"rules",Json.raw("["+String.join(",",rules)+"]"),"final",s.processes.isEmpty()?"proxy":"direct")));}
}
