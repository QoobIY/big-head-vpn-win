package app.bighead.vpn;

import java.util.*;

final class Json {
    private Json() {}
    static String quote(String value) {
        var out = new StringBuilder("\"");
        for (char c : value.toCharArray()) switch (c) {
            case '\"' -> out.append("\\\""); case '\\' -> out.append("\\\\"); case '\n' -> out.append("\\n");
            case '\r' -> out.append("\\r"); case '\t' -> out.append("\\t");
            default -> { if (c < 32) out.append(String.format("\\u%04x", (int)c)); else out.append(c); }
        }
        return out.append('\"').toString();
    }
    static String object(Object... pairs) {
        var parts = new ArrayList<String>();
        for (int i=0; i<pairs.length; i+=2) if (pairs[i+1] != null) parts.add(quote((String)pairs[i])+":"+value(pairs[i+1]));
        return "{"+String.join(",",parts)+"}";
    }
    static String array(Collection<?> values) { return "["+values.stream().map(Json::value).reduce((a,b)->a+","+b).orElse("")+"]"; }
    static Raw raw(String json) { return new Raw(json); }
    private static String value(Object value) {
        if (value instanceof Raw r) return r.json; if (value instanceof String s) return quote(s);
        if (value instanceof Boolean || value instanceof Number) return value.toString();
        if (value instanceof Collection<?> c) return array(c); return quote(value.toString());
    }
    record Raw(String json) {}
}
