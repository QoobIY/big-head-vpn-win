package app.bighead.vpn;

import javax.swing.*;
import javax.swing.Timer;
import javax.swing.border.*;
import java.awt.*;
import java.awt.datatransfer.DataFlavor;
import java.awt.datatransfer.StringSelection;
import java.net.*;
import java.net.http.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.io.IOException;
import java.nio.file.*;
import java.nio.channels.*;
import java.time.Duration;
import java.util.List;
import java.util.*;
import java.util.regex.Pattern;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.atomic.AtomicInteger;

public final class Main {
    static FileChannel instanceChannel; static FileLock instanceLock;
    static final Color BG=new Color(49,51,56), SIDEBAR=new Color(30,31,34), PANEL=new Color(43,45,49), INPUT=new Color(30,31,34), BLURPLE=new Color(88,101,242), GREEN=new Color(35,165,90), TEXT=new Color(242,243,245), MUTED=new Color(181,186,193);
    public static void main(String[] args){var settings=Settings.load();if(Settings.MODE_TUN.equals(settings.mode)&&!ensureWindowsAdministrator())return;if(!acquireSingleInstance())return;SwingUtilities.invokeLater(()->{theme();new Window(settings).setVisible(true);});}
    static boolean acquireSingleInstance(){try{Files.createDirectories(Settings.DIR);instanceChannel=FileChannel.open(Settings.DIR.resolve("app.lock"),StandardOpenOption.CREATE,StandardOpenOption.WRITE);instanceLock=instanceChannel.tryLock();if(instanceLock==null){instanceChannel.close();return false;}Runtime.getRuntime().addShutdownHook(new Thread(()->{try{instanceLock.release();instanceChannel.close();}catch(Exception ignored){}}));return true;}catch(OverlappingFileLockException e){return false;}catch(Exception e){return true;}}
    static boolean isWindowsAdministrator(){if(!System.getProperty("os.name","").startsWith("Windows"))return true;try{var test="if ((New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { exit 0 } else { exit 1 }";var check=new ProcessBuilder("powershell.exe","-NoProfile","-NonInteractive","-Command",test).start();return check.waitFor(3,java.util.concurrent.TimeUnit.SECONDS)&&check.exitValue()==0;}catch(Exception e){return false;}}
    static boolean ensureWindowsAdministrator(){if(isWindowsAdministrator())return true;try{var info=ProcessHandle.current().info();var executable=info.command().orElse("");if(executable.isBlank())return false;var escaped=executable.replace("'","''");var parent=Path.of(executable).toAbsolutePath().getParent().toString().replace("'","''");var args=Arrays.stream(info.arguments().orElse(new String[0])).map(value->"'"+value.replace("'","''")+"'").toList();var argumentList=args.isEmpty()?"":" -ArgumentList @("+String.join(",",args)+")";new ProcessBuilder("powershell.exe","-NoProfile","-WindowStyle","Hidden","-Command","Start-Process -FilePath '"+escaped+"' -WorkingDirectory '"+parent+"'"+argumentList+" -Verb RunAs").start();return false;}catch(Exception e){return false;}}
    static void releaseSingleInstance(){try{if(instanceLock!=null&&instanceLock.isValid())instanceLock.release();}catch(Exception ignored){}try{if(instanceChannel!=null&&instanceChannel.isOpen())instanceChannel.close();}catch(Exception ignored){}}
    private static void theme(){var f=new Font("Segoe UI",Font.PLAIN,14);UIManager.put("defaultFont",f);UIManager.put("Panel.background",BG);UIManager.put("Label.foreground",TEXT);UIManager.put("Button.background",BLURPLE);UIManager.put("Button.foreground",Color.WHITE);UIManager.put("TextField.background",INPUT);UIManager.put("TextField.foreground",TEXT);UIManager.put("TextField.caretForeground",TEXT);UIManager.put("List.background",PANEL);UIManager.put("List.foreground",MUTED);UIManager.put("List.selectionBackground",new Color(64,66,73));UIManager.put("List.selectionForeground",TEXT);UIManager.put("TextArea.background",INPUT);UIManager.put("TextArea.foreground",MUTED);UIManager.put("ComboBox.background",INPUT);UIManager.put("ComboBox.foreground",TEXT);UIManager.put("ComboBox.selectionBackground",new Color(64,66,73));UIManager.put("ComboBox.selectionForeground",TEXT);UIManager.put("ScrollPane.border",new EmptyBorder(0,0,0,0));UIManager.put("CheckBox.background",PANEL);UIManager.put("CheckBox.foreground",TEXT);UIManager.put("ToolTip.background",PANEL);UIManager.put("ToolTip.foreground",TEXT);UIManager.put("OptionPane.background",PANEL);UIManager.put("OptionPane.messageForeground",TEXT);UIManager.put("OptionPane.foreground",TEXT);}

    @SuppressWarnings("serial")
    static final class Window extends JFrame {
        enum ConnectionState { OFF, STARTING, ON, STOPPING }
        final Settings settings; final DefaultListModel<Profile> profiles=new DefaultListModel<>(); final DefaultComboBoxModel<Object> groups=new DefaultComboBoxModel<>();
        final JList<Profile> profilesList=new JList<>(profiles);
        final JComboBox<Object> groupBox=new JComboBox<>(groups); final JComboBox<String> modeBox=new JComboBox<>(new String[]{"VPN для всей системы (TUN)","Прокси-сервер (SOCKS5 + HTTP)"}); final JLabel modeHelp=new JLabel();
        final JLabel status=new JLabel("Отключено"),detail=new JLabel("Выберите сервер"),dot=new JLabel("●"); final JTextArea alert=new JTextArea(); final JButton connect=new DiscordButton("Включить",true);
        final JCheckBox listener=new JCheckBox("Дополнительно включить прокси-сервер"); final JTextField address=new JTextField(),port=new JTextField(),profileInput=new JTextField(); final JTextArea logs=new JTextArea(); final JLabel listenerWarning=new JLabel();
        final ConcurrentLinkedQueue<String> pendingLogs=new ConcurrentLinkedQueue<>(); final AtomicInteger pendingLogCount=new AtomicInteger(); final Timer logTimer;
        final SingBox engine=new SingBox(this::log,this::engineStopped); long startedAt,pendingDeleteUntil; String pendingDeleteGroupId=""; volatile boolean restarting; volatile String runningProfileId=""; volatile ConnectionState connectionState=ConnectionState.OFF;
        Window(Settings settings){super("Big Head VPN");this.settings=settings;logTimer=new Timer(150,e->flushLogs());logTimer.setCoalesce(true);logTimer.start();var logo=logo(512);setIconImages(List.of(logo,scale(logo,256),scale(logo,64),scale(logo,32),scale(logo,16)));var dialogIcon=new ImageIcon(scale(logo,64));UIManager.put("OptionPane.informationIcon",dialogIcon);UIManager.put("OptionPane.questionIcon",dialogIcon);UIManager.put("OptionPane.warningIcon",dialogIcon);UIManager.put("OptionPane.errorIcon",dialogIcon);setDefaultCloseOperation(DO_NOTHING_ON_CLOSE);setMinimumSize(new Dimension(1000,650));setSize(1180,760);setLocationRelativeTo(null);groups.addElement("Все серверы");settings.groups.forEach(groups::addElement);listener.setSelected(settings.listenerEnabled);address.setText(settings.listenAddress);port.setText(Integer.toString(settings.listenPort));modeBox.setSelectedIndex(Settings.MODE_PROXY.equals(settings.mode)?1:0);build();updateModeControls();events();refreshProfileView();selectSaved();}
        void build(){var root=new JPanel(new BorderLayout());root.add(sidebar(),BorderLayout.WEST);var workspace=new JPanel(new BorderLayout());alert.setOpaque(true);alert.setEditable(false);alert.setFocusable(false);alert.setLineWrap(true);alert.setWrapStyleWord(true);alert.setRows(2);alert.setBackground(new Color(218,55,60));alert.setForeground(Color.WHITE);alert.setFont(alert.getFont().deriveFont(Font.BOLD));alert.setBorder(new EmptyBorder(10,16,10,16));alert.setVisible(false);workspace.add(alert,BorderLayout.NORTH);var content=new JPanel(new GridLayout(1,2,18,0));content.setBorder(new EmptyBorder(24,26,24,26));content.add(columnScroll(left()));content.add(columnScroll(right()));workspace.add(content);root.add(workspace);setContentPane(root);decorate();}
        JScrollPane columnScroll(JPanel panel){var scroll=new JScrollPane(panel);scroll.setHorizontalScrollBarPolicy(ScrollPaneConstants.HORIZONTAL_SCROLLBAR_NEVER);scroll.setVerticalScrollBarPolicy(ScrollPaneConstants.VERTICAL_SCROLLBAR_AS_NEEDED);scroll.getVerticalScrollBar().setUnitIncrement(18);scroll.getViewport().setBackground(BG);return scroll;}
        JPanel sidebar(){var p=new JPanel();p.setBackground(SIDEBAR);p.setPreferredSize(new Dimension(82,0));p.setLayout(new BoxLayout(p,BoxLayout.Y_AXIS));p.setBorder(new EmptyBorder(18,12,18,12));var logoLabel=new JLabel(new ImageIcon(scale(logo(512),58)),SwingConstants.CENTER);logoLabel.setToolTipText("Big Head VPN");logoLabel.setMaximumSize(new Dimension(58,58));logoLabel.setPreferredSize(new Dimension(58,58));logoLabel.setAlignmentX(.5f);p.add(logoLabel);return p;}
        Image logo(int size){try(var in=Main.class.getResourceAsStream("/assets/icon.png")){if(in==null)throw new IOException("logo resource missing");return javax.imageio.ImageIO.read(in).getScaledInstance(size,size,Image.SCALE_SMOOTH);}catch(IOException e){log("Logo: "+e);return new java.awt.image.BufferedImage(size,size,java.awt.image.BufferedImage.TYPE_INT_ARGB);}}
        Image scale(Image image,int size){return image.getScaledInstance(size,size,Image.SCALE_SMOOTH);}
        JPanel left(){
            var panel=vertical();
            var title=new JLabel("Big Head VPN");
            title.setFont(title.getFont().deriveFont(Font.BOLD,27));
            panel.add(title);
            panel.add(muted("VLESS  •  Hysteria2  •  Windows"));
            panel.add(Box.createVerticalStrut(15));

            var state=new DiscordCard(new BorderLayout(12,10));
            dot.setForeground(new Color(128,132,142));
            dot.setFont(dot.getFont().deriveFont(26f));
            status.setFont(status.getFont().deriveFont(Font.BOLD,21));
            var stateLine=transparent(new BorderLayout(8,0));
            stateLine.add(dot,BorderLayout.WEST);
            stateLine.add(status);
            state.add(stateLine,BorderLayout.NORTH);
            state.add(detail);
            connect.setPreferredSize(new Dimension(120,46));
            state.add(connect,BorderLayout.SOUTH);
            panel.add(state);

            panel.add(section("СЕРВЕРЫ И ПОДПИСКИ"));
            var servers=new DiscordCard(new BorderLayout(0,8));
            var groupLine=transparent(new BorderLayout(6,0));
            groupLine.add(groupBox);
            var groupActions=flow();
            groupActions.add(button("Обновить",this::refreshGroup));
            groupActions.add(button("Удалить группу",this::deleteGroup));
            groupLine.add(groupActions,BorderLayout.EAST);
            servers.add(groupLine,BorderLayout.NORTH);
            profilesList.setVisibleRowCount(6);
            var profileScroll=new JScrollPane(profilesList);
            profileScroll.setHorizontalScrollBarPolicy(ScrollPaneConstants.HORIZONTAL_SCROLLBAR_NEVER);
            servers.add(profileScroll);
            var bottom=verticalClear();
            var inputLine=transparent(new BorderLayout(6,0));
            profileInput.setToolTipText("Вставьте vless://, hysteria2:// или HTTPS-ссылку подписки");
            inputLine.add(profileInput);
            inputLine.add(button("Добавить",this::manualProfile),BorderLayout.EAST);
            bottom.add(inputLine);
            var buttons=flow();
            buttons.add(button("Вставить из буфера",this::paste));
            buttons.add(button("Удалить сервер",this::deleteProfile));
            bottom.add(buttons);
            servers.add(bottom,BorderLayout.SOUTH);
            panel.add(servers);

            panel.add(section("ПРОКСИ-СЕРВЕР"));
            panel.add(proxySettingsCard());
            return panel;
        }

        JPanel proxySettingsCard(){
            var card=new DiscordCard(new BorderLayout(8,10));
            card.add(listener,BorderLayout.NORTH);
            var fields=transparent(new GridLayout(1,2,10,0));
            fields.add(labeledField("IP-АДРЕС",address));
            fields.add(labeledField("ПОРТ",port));
            card.add(fields);
            var footer=verticalClear();
            listenerWarning.setBorder(new EmptyBorder(8,2,2,2));
            footer.add(listenerWarning);
            footer.add(muted("SOCKS5 и HTTP используют один адрес и порт. Авторизация не настроена."));
            card.add(footer,BorderLayout.SOUTH);
            return card;
        }

        JPanel labeledField(String title,JComponent component){
            var panel=verticalClear();
            var label=new JLabel(title);
            label.setForeground(MUTED);
            label.setFont(label.getFont().deriveFont(Font.BOLD,11));
            label.setBorder(new EmptyBorder(0,2,5,2));
            panel.add(label);
            panel.add(component);
            return panel;
        }

        JPanel right(){
            var panel=vertical();
            panel.add(section("РЕЖИМ РАБОТЫ"));
            var modeCard=new DiscordCard(new BorderLayout(8,12));
            modeBox.setPreferredSize(new Dimension(300,42));
            modeCard.add(modeBox,BorderLayout.NORTH);
            modeHelp.setForeground(MUTED);
            modeHelp.setBorder(new EmptyBorder(8,3,8,3));
            modeCard.add(modeHelp);
            panel.add(modeCard);

            panel.add(section("КАК ЭТО РАБОТАЕТ"));
            var info=new DiscordCard(new BorderLayout());
            info.add(muted("<b>Системный VPN</b> создаёт TUN и направляет весь TCP/UDP-трафик Windows через выбранный сервер.<br><br><b>Прокси-сервер</b> не меняет маршруты. Укажите его SOCKS5/HTTP-адрес в нужном приложении или на другом устройстве."));
            panel.add(info);

            var journalHeader=transparent(new BorderLayout());
            journalHeader.add(section("ЖУРНАЛ"));
            var journalActions=flow();
            journalActions.add(button("Копировать",this::copyLogs));
            journalActions.add(button("Очистить",()->logs.setText("")));
            journalHeader.add(journalActions,BorderLayout.EAST);
            panel.add(journalHeader);
            logs.setEditable(false);
            logs.setLineWrap(true);
            logs.setWrapStyleWord(true);
            logs.setRows(12);
            logs.setMargin(new Insets(8,8,8,8));
            var scroll=new JScrollPane(logs);
            scroll.setPreferredSize(new Dimension(400,260));
            panel.add(scroll);
            return panel;
        }
        void decorate(){var renderer=new DefaultListCellRenderer(){public Component getListCellRendererComponent(JList<?> l,Object v,int i,boolean s,boolean f){var c=(JLabel)super.getListCellRendererComponent(l,v,i,s,f);c.setBorder(new EmptyBorder(9,10,9,10));return c;}};profilesList.setCellRenderer(renderer);for(var x:List.of(address,port,profileInput)){x.setBorder(new CompoundBorder(new LineBorder(INPUT),new EmptyBorder(9,10,9,10)));x.setPreferredSize(new Dimension(100,38));}}
        JPanel vertical(){var p=new JPanel();p.setLayout(new BoxLayout(p,BoxLayout.Y_AXIS));return p;} JPanel verticalClear(){var p=vertical();p.setOpaque(false);return p;} JPanel transparent(LayoutManager l){var p=new JPanel(l);p.setOpaque(false);return p;} JPanel flow(){return transparent(new FlowLayout(FlowLayout.LEFT,4,2));} JLabel muted(String text){var l=new JLabel("<html>"+text+"</html>");l.setForeground(MUTED);l.setBorder(new EmptyBorder(5,2,7,2));return l;} JLabel section(String text){var l=new JLabel(text);l.setForeground(MUTED);l.setFont(l.getFont().deriveFont(Font.BOLD,12));l.setBorder(new EmptyBorder(18,2,7,2));return l;} JButton button(String text,Runnable run){var b=new DiscordButton(text,false);b.addActionListener(e->run.run());return b;}
        void events(){
            connect.addActionListener(e->toggle());
            groupBox.addActionListener(e->refreshProfileView());
            modeBox.addActionListener(e->changeMode());
            listener.addActionListener(e->{
                settings.listenerEnabled=listener.isSelected();
                updateListenerWarning();
                save();
                if(connectionState==ConnectionState.ON)restartVpn("Применение настроек прокси…");
            });
            address.addActionListener(e->applyNetworkSettings());
            port.addActionListener(e->applyNetworkSettings());
            profileInput.addActionListener(e->manualProfile());
            profilesList.addListSelectionListener(e->{
                if(e.getValueIsAdjusting())return;
                var profile=profilesList.getSelectedValue();
                settings.selectedId=profile==null?"":profile.id();
                save();
                render();
                if(connectionState==ConnectionState.ON&&profile!=null&&!profile.id().equals(runningProfileId))restartVpn("Переключение сервера…");
            });
            getRootPane().registerKeyboardAction(e->clearError(),KeyStroke.getKeyStroke("ESCAPE"),JComponent.WHEN_IN_FOCUSED_WINDOW);
            getRootPane().registerKeyboardAction(e->copyLogs(),KeyStroke.getKeyStroke("control shift C"),JComponent.WHEN_IN_FOCUSED_WINDOW);
            alert.setToolTipText("Нажмите, чтобы скрыть сообщение");
            alert.addMouseListener(new java.awt.event.MouseAdapter(){public void mouseClicked(java.awt.event.MouseEvent e){clearError();}});
            addWindowListener(new java.awt.event.WindowAdapter(){public void windowClosing(java.awt.event.WindowEvent e){setEnabled(false);setVisible(false);Thread.ofVirtual().start(()->{try{engine.close();save();}finally{System.exit(0);}});Thread.ofVirtual().start(()->{try{Thread.sleep(3500);}catch(InterruptedException ignored){}System.exit(0);});}});
        }
        void toggle(){if(connectionState==ConnectionState.STARTING||connectionState==ConnectionState.STOPPING)return;if(connectionState==ConnectionState.ON||engine.running()){setConnectionState(ConnectionState.STOPPING);Thread.ofVirtual().start(()->{engine.stop();startedAt=0;runningProfileId="";SwingUtilities.invokeLater(()->setConnectionState(ConnectionState.OFF));});return;}var p=profilesList.getSelectedValue();if(p==null){error("Сначала добавьте и выберите сервер.");return;}if(!readListener())return;if(Settings.MODE_TUN.equals(settings.mode)&&!isWindowsAdministrator()){relaunchElevated();return;}clearError();setConnectionState(ConnectionState.STARTING);Thread.ofVirtual().start(()->{try{engine.start(p,settings);startedAt=System.currentTimeMillis();runningProfileId=p.id();SwingUtilities.invokeLater(()->setConnectionState(ConnectionState.ON));}catch(Exception ex){runningProfileId="";log(ex.getMessage());SwingUtilities.invokeLater(()->{setConnectionState(ConnectionState.OFF);error(friendlyError(ex));});}});}
        void relaunchElevated(){notice("Для системного TUN нужны права администратора. Перезапускаю приложение…");setEnabled(false);Thread.ofVirtual().start(()->{try{engine.close();save();releaseSingleInstance();ensureWindowsAdministrator();}finally{System.exit(0);}});}
        void restartVpn(String message){if(restarting||connectionState==ConnectionState.STARTING||connectionState==ConnectionState.STOPPING)return;var p=profilesList.getSelectedValue();if(p==null)return;restarting=true;setConnectionState(ConnectionState.STARTING);status.setText(message);Thread.ofVirtual().start(()->{try{engine.stop();engine.start(p,settings);startedAt=System.currentTimeMillis();runningProfileId=p.id();log("Подключение перезапущено: "+p.name());SwingUtilities.invokeLater(()->setConnectionState(ConnectionState.ON));}catch(Exception ex){runningProfileId="";log(ex.getMessage());SwingUtilities.invokeLater(()->{setConnectionState(ConnectionState.OFF);error(friendlyError(ex));});}finally{restarting=false;}});}
        void engineStopped(String reason){SwingUtilities.invokeLater(()->{startedAt=0;if(!restarting&&connectionState==ConnectionState.ON){runningProfileId="";setConnectionState(ConnectionState.OFF);if(reason!=null&&!reason.isBlank())error("Сетевое ядро остановлено: "+reason);}});}
        void setConnectionState(ConnectionState next){connectionState=next;boolean busy=next==ConnectionState.STARTING||next==ConnectionState.STOPPING;connect.setEnabled(!busy);modeBox.setEnabled(!busy);profilesList.setEnabled(!busy);groupBox.setEnabled(!busy);status.setText(switch(next){case OFF->"Отключено";case STARTING->"Подключение…";case ON->"Подключено";case STOPPING->"Отключение…";});dot.setForeground(next==ConnectionState.ON?GREEN:next==ConnectionState.STARTING?new Color(240,178,50):new Color(128,132,142));connect.setText(next==ConnectionState.ON?"Выключить":"Включить");render();}
        String friendlyError(Exception error){var message=error.getMessage();if(message==null||message.isBlank())return "Неизвестная ошибка: "+error.getClass().getSimpleName();return message.length()>500?message.substring(0,500)+"…":message;}
        void render(){boolean on=connectionState==ConnectionState.ON;var p=profilesList.getSelectedValue();var suffix=on?(Settings.MODE_PROXY.equals(settings.mode)?" · прокси активен":" · системный TUN активен"):"";detail.setText(p==null?"Выберите сервер":p.name()+suffix);}
        void paste(){try{String text=null;Exception cause=null;for(int i=0;i<4&&text==null;i++)try{var cb=Toolkit.getDefaultToolkit().getSystemClipboard();if(!cb.isDataFlavorAvailable(DataFlavor.stringFlavor))throw new IllegalStateException("в буфере нет текста");text=(String)cb.getData(DataFlavor.stringFlavor);}catch(Exception e){cause=e;Thread.sleep(70);}if(text==null)throw cause;importText(text);}catch(Exception e){log("Clipboard error: "+e);error("Не удалось прочитать ссылку из буфера: "+e.getMessage());}}
        void manualProfile(){var text=profileInput.getText().trim();if(text.isEmpty()){error("Вставьте VPN URI или HTTPS-ссылку подписки в поле добавления");profileInput.requestFocusInWindow();return;}profileInput.setText("");importText(text);}
        void importText(String text){var uris=extractUris(text);if(!uris.isEmpty()){addProfiles(uris,"");return;}var m=Pattern.compile("(?i)https?://[^\\s<>\\\"']+").matcher(text.trim());if(!m.find()){error("Не найдены VPN-ссылки или HTTPS-ссылка подписки");return;}var url=m.group().replaceAll("[),.;]+$","");var existing=settings.groups.stream().filter(g->g.url().equals(url)).findFirst().orElse(null);fetchSubscription(url,existing);}
        void fetchSubscription(String url,SubscriptionGroup existing){clearError();status.setText(existing==null?"Добавление подписки…":"Обновление подписки…");connect.setEnabled(false);log("Загрузка подписки: "+URI.create(url).getHost());Thread.ofVirtual().start(()->{try{var request=HttpRequest.newBuilder(URI.create(url)).timeout(Duration.ofSeconds(20)).header("User-Agent","BigHeadVPN/0.1").GET().build();var client=HttpClient.newBuilder().followRedirects(HttpClient.Redirect.NORMAL).connectTimeout(Duration.ofSeconds(12)).build();var response=client.send(request,HttpResponse.BodyHandlers.ofByteArray());log("Подписка HTTP "+response.statusCode()+", "+response.body().length+" байт");if(response.statusCode()<200||response.statusCode()>299)throw new IllegalStateException("HTTP "+response.statusCode());if(response.body().length>2_000_000)throw new IllegalStateException("ответ больше 2 МБ");var body=new String(response.body(),StandardCharsets.UTF_8);var found=extractUris(body);if(found.isEmpty())try{body=new String(Base64.getMimeDecoder().decode(body.trim()),StandardCharsets.UTF_8);found=extractUris(body);}catch(IllegalArgumentException ignored){}if(found.isEmpty())throw new IllegalStateException("серверы в ответе не найдены");var name=subscriptionName(body,url);var result=List.copyOf(found);SwingUtilities.invokeLater(()->applySubscription(existing,name,url,result));}catch(Exception e){log("Subscription error: "+e);SwingUtilities.invokeLater(()->error("Не удалось загрузить подписку: "+e.getClass().getSimpleName()+": "+e.getMessage()));}finally{SwingUtilities.invokeLater(()->{connect.setEnabled(true);render();});}});}
        String subscriptionName(String body,String url){var m=Pattern.compile("(?im)^#profile-title:\\s*(.+)$").matcher(body);if(m.find()&&!m.group(1).isBlank())return m.group(1).trim();try{return URI.create(url).getHost();}catch(Exception e){return "Подписка";}}
        void applySubscription(SubscriptionGroup existing,String name,String url,List<String> values){var group=existing==null?new SubscriptionGroup(name,url):new SubscriptionGroup(existing.id(),name,url);var parsed=new ArrayList<Profile>();var errors=new LinkedHashSet<String>();for(var value:values)try{var p=ProfileParser.parse(value);ProfileParser.outbound(p);parsed.add(p.inGroup(group.id()));}catch(Exception e){errors.add(e.getMessage());}if(parsed.isEmpty()){error("В подписке нет поддерживаемых серверов");return;}if(existing==null){settings.groups.add(group);groups.addElement(group);}else{var index=settings.groups.indexOf(existing);if(index>=0)settings.groups.set(index,group);for(int i=1;i<groups.getSize();i++)if(groups.getElementAt(i) instanceof SubscriptionGroup g&&g.id().equals(group.id()))groups.removeElementAt(i--);}settings.profiles.removeIf(p->p.groupId().equals(group.id()));settings.profiles.addAll(parsed);if(existing!=null)groups.addElement(group);groupBox.setSelectedItem(group);save();refreshProfileView();selectSaved();log((existing==null?"Добавлена":"Обновлена")+" подписка '"+name+"': "+parsed.size()+" серверов");if(!errors.isEmpty())warning("Добавлено серверов: "+parsed.size()+". Часть серверов пропущена:\n"+String.join("\n",errors));}
        List<String> extractUris(String text){var result=new ArrayList<String>();var m=Pattern.compile("(?i)(?:vless|hysteria2?|hy2)://[^\\s<>\\\"']+").matcher(text);while(m.find())result.add(m.group().replaceAll("[),.;]+$",""));return result;}
        void addProfiles(Collection<String> values,String groupId){if(values.isEmpty()){error("В буфере не найдены ссылки vless://, hysteria://, hysteria2:// или hy2://");return;}var errors=new LinkedHashSet<String>();int count=0;for(var value:values)try{var p=ProfileParser.parse(value).inGroup(groupId);ProfileParser.outbound(p);if(settings.profiles.stream().noneMatch(x->x.uri().equals(p.uri()))){settings.profiles.add(p);count++;}}catch(Exception e){errors.add(e.getMessage());}refreshProfileView();if(profilesList.getSelectedIndex()<0&&profiles.size()>0)profilesList.setSelectedIndex(0);save();log("Импортировано серверов: "+count);if(!errors.isEmpty())warning("Добавлено серверов: "+count+". Часть серверов пропущена:\n"+String.join("\n",errors));}
        void refreshProfileView(){if(profiles==null)return;var chosen=groupBox.getSelectedItem();var selectedId=settings.selectedId;profiles.clear();settings.profiles.stream().filter(p->!(chosen instanceof SubscriptionGroup g)||p.groupId().equals(g.id())).forEach(profiles::addElement);for(int i=0;i<profiles.size();i++)if(profiles.get(i).id().equals(selectedId)){profilesList.setSelectedIndex(i);break;}render();}
        void refreshGroup(){if(connectionState!=ConnectionState.OFF){warning("Отключите VPN перед обновлением подписки, чтобы активный сервер не был заменён во время соединения.");return;}if(groupBox.getSelectedItem() instanceof SubscriptionGroup g)fetchSubscription(g.url(),g);else error("Выберите группу подписки");}
        void deleteGroup(){if(!(groupBox.getSelectedItem() instanceof SubscriptionGroup g)){error("Выберите группу подписки");return;}if(engine.running()){error("Сначала отключите VPN.");return;}var now=System.currentTimeMillis();if(!pendingDeleteGroupId.equals(g.id())||now>pendingDeleteUntil){pendingDeleteGroupId=g.id();pendingDeleteUntil=now+6000;warning("Чтобы удалить группу '"+g.name()+"' и все её серверы, нажмите «Удалить группу» ещё раз");return;}pendingDeleteGroupId="";pendingDeleteUntil=0;settings.profiles.removeIf(p->p.groupId().equals(g.id()));settings.groups.removeIf(x->x.id().equals(g.id()));groups.removeElement(g);groupBox.setSelectedIndex(0);save();refreshProfileView();clearError();log("Группа удалена: "+g.name());}
        void deleteProfile(){var p=profilesList.getSelectedValue();if(p==null)return;if(engine.running()){error("Сначала отключите VPN.");return;}settings.profiles.remove(p);profiles.removeElement(p);if(!profiles.isEmpty())profilesList.setSelectedIndex(0);save();}
        void selectSaved(){for(int i=0;i<profiles.size();i++)if(profiles.get(i).id().equals(settings.selectedId)){profilesList.setSelectedIndex(i);return;}if(!profiles.isEmpty())profilesList.setSelectedIndex(0);render();}
        void changeMode(){settings.mode=modeBox.getSelectedIndex()==1?Settings.MODE_PROXY:Settings.MODE_TUN;if(Settings.MODE_PROXY.equals(settings.mode))listener.setSelected(true);settings.listenerEnabled=listener.isSelected();updateModeControls();save();if(connectionState==ConnectionState.ON){if(Settings.MODE_TUN.equals(settings.mode)&&!isWindowsAdministrator())relaunchElevated();else restartVpn("Переключение режима…");}}
        void updateModeControls(){var proxy=Settings.MODE_PROXY.equals(settings.mode);listener.setEnabled(!proxy);if(proxy)listener.setSelected(true);modeHelp.setText(proxy?"<html><b>Только прокси-сервер.</b><br>TUN и системные маршруты не создаются.</html>":"<html><b>Полный системный VPN.</b><br>Весь трафик Windows направляется через TUN.</html>");updateListenerWarning();}
        void updateListenerWarning(){var active=Settings.MODE_PROXY.equals(settings.mode)||listener.isSelected();var value=address.getText().trim();var exposed=value.equals("0.0.0.0")||value.equals("::")||value.equals("[::]");if(!active){listenerWarning.setForeground(MUTED);listenerWarning.setText("Прокси-сервер выключен в этом режиме.");}else if(exposed){listenerWarning.setForeground(new Color(240,178,50));listenerWarning.setText("⚠ Доступ открыт для локальной сети без пароля.");}else{listenerWarning.setForeground(GREEN);listenerWarning.setText("● Доступ ограничен адресом "+(value.isBlank()?"127.0.0.1":value)+".");}}
        void applyNetworkSettings(){if(readListener()&&connectionState==ConnectionState.ON)restartVpn("Применение сетевых настроек…");updateListenerWarning();}
        boolean readListener(){var active=Settings.MODE_PROXY.equals(settings.mode)||listener.isSelected();if(!active){settings.listenerEnabled=false;save();return true;}try{var host=address.getText().trim();if(host.isBlank())throw new Exception();InetAddress.getByName(host);int n=Integer.parseInt(port.getText().trim());if(n<1||n>65535)throw new Exception();settings.listenerEnabled=true;settings.listenAddress=host;settings.listenPort=n;save();return true;}catch(Exception e){error("Укажите корректный IP-адрес и порт от 1 до 65535.");return false;}}
        void copyLogs(){var text=logs.getText();if(text.isBlank())return;Toolkit.getDefaultToolkit().getSystemClipboard().setContents(new StringSelection(text),null);notice("Журнал скопирован в буфер обмена");}
        void save(){try{settings.save();}catch(RuntimeException e){log(e.getMessage());}}
        void updateAlert(){alert.getParent().revalidate();alert.getParent().repaint();}
        void clearError(){alert.setVisible(false);alert.setText("");updateAlert();}
        void showAlert(String text,Color background,Color foreground){int lines=1;for(int i=0;i<text.length();i++)if(text.charAt(i)=='\n')lines++;lines+=text.length()/110;alert.setRows(Math.max(1,Math.min(4,lines)));alert.setBackground(background);alert.setForeground(foreground);alert.setText(text);alert.setCaretPosition(0);alert.setVisible(true);updateAlert();}
        void notice(String text){showAlert(text,GREEN,Color.WHITE);}
        void warning(String text){showAlert("Предупреждение: "+text,new Color(240,178,50),SIDEBAR);log("Предупреждение: "+text);}
        void error(String text){showAlert("Ошибка: "+text,new Color(218,55,60),Color.WHITE);log("Ошибка: "+text);render();}
        void log(String s){if(s==null||s.isBlank())return;while(pendingLogCount.get()>=2000){if(pendingLogs.poll()!=null)pendingLogCount.decrementAndGet();else break;}pendingLogs.offer(s);pendingLogCount.incrementAndGet();}
        void flushLogs(){var batch=new StringBuilder();for(int i=0;i<200;i++){var line=pendingLogs.poll();if(line==null)break;pendingLogCount.decrementAndGet();batch.append(line).append('\n');}if(batch.isEmpty())return;logs.append(batch.toString());var doc=logs.getDocument();if(doc.getLength()>100_000)try{doc.remove(0,doc.getLength()-80_000);}catch(javax.swing.text.BadLocationException ignored){}logs.setCaretPosition(doc.getLength());}
    }
    @SuppressWarnings("serial")
    static final class DiscordCard extends JPanel {DiscordCard(LayoutManager l){super(l);setBackground(PANEL);setBorder(new EmptyBorder(14,14,14,14));}}
    static final class DiscordButton extends JButton {private static final long serialVersionUID=1L;final boolean primary;DiscordButton(String text,boolean primary){super(text);this.primary=primary;setOpaque(false);setContentAreaFilled(false);setBorder(new EmptyBorder(10,14,10,14));setForeground(Color.WHITE);setCursor(Cursor.getPredefinedCursor(Cursor.HAND_CURSOR));}protected void paintComponent(Graphics g){var g2=(Graphics2D)g.create();g2.setRenderingHint(RenderingHints.KEY_ANTIALIASING,RenderingHints.VALUE_ANTIALIAS_ON);g2.setColor(primary?BLURPLE:(getModel().isRollover()?new Color(78,80,88):new Color(64,66,73)));g2.fillRoundRect(0,0,getWidth(),getHeight(),8,8);if(isFocusOwner()){g2.setColor(new Color(190,195,255));g2.setStroke(new BasicStroke(2));g2.drawRoundRect(2,2,getWidth()-5,getHeight()-5,7,7);}g2.dispose();super.paintComponent(g);}}
}
