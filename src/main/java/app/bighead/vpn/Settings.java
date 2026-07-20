package app.bighead.vpn;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

public final class Settings {
    public final List<Profile> profiles=new ArrayList<>(); public final List<SubscriptionGroup> groups=new ArrayList<>(); public final List<String> processes=new ArrayList<>();
    public String selectedId="", listenAddress="0.0.0.0"; public int listenPort=2080; public boolean listenerEnabled=true;
    public static final Path DIR=Path.of(System.getenv().getOrDefault("LOCALAPPDATA",System.getProperty("user.home")),"BigHeadVpn");
    private static final Path FILE=DIR.resolve("settings.properties");
    public static Settings load(){var s=new Settings();var p=new Properties();try(var in=Files.newInputStream(FILE)){p.loadFromXML(in);s.selectedId=p.getProperty("selected","");s.listenAddress=p.getProperty("listenAddress","0.0.0.0");s.listenPort=Integer.parseInt(p.getProperty("listenPort","2080"));s.listenerEnabled=Boolean.parseBoolean(p.getProperty("listenerEnabled","true"));
        for(int i=0;i<Integer.parseInt(p.getProperty("profiles","0"));i++)s.profiles.add(new Profile(p.getProperty("p."+i+".id"),p.getProperty("p."+i+".name"),p.getProperty("p."+i+".uri"),p.getProperty("p."+i+".group","")));
        for(int i=0;i<Integer.parseInt(p.getProperty("groups","0"));i++)s.groups.add(new SubscriptionGroup(p.getProperty("g."+i+".id"),p.getProperty("g."+i+".name"),p.getProperty("g."+i+".url")));
        for(int i=0;i<Integer.parseInt(p.getProperty("processes","0"));i++)s.processes.add(p.getProperty("process."+i));}catch(Exception ignored){}return s;}
    public void save(){try{Files.createDirectories(DIR);var p=new Properties();p.setProperty("selected",selectedId);p.setProperty("listenAddress",listenAddress);p.setProperty("listenPort",Integer.toString(listenPort));p.setProperty("listenerEnabled",Boolean.toString(listenerEnabled));p.setProperty("profiles",Integer.toString(profiles.size()));for(int i=0;i<profiles.size();i++){var x=profiles.get(i);p.setProperty("p."+i+".id",x.id());p.setProperty("p."+i+".name",x.name());p.setProperty("p."+i+".uri",x.uri());p.setProperty("p."+i+".group",x.groupId());}p.setProperty("groups",Integer.toString(groups.size()));for(int i=0;i<groups.size();i++){var x=groups.get(i);p.setProperty("g."+i+".id",x.id());p.setProperty("g."+i+".name",x.name());p.setProperty("g."+i+".url",x.url());}p.setProperty("processes",Integer.toString(processes.size()));for(int i=0;i<processes.size();i++)p.setProperty("process."+i,processes.get(i));try(var out=Files.newOutputStream(FILE)){p.storeToXML(out,"Big Head VPN settings",StandardCharsets.UTF_8);}}catch(IOException e){throw new UncheckedIOException(e);}}
}
