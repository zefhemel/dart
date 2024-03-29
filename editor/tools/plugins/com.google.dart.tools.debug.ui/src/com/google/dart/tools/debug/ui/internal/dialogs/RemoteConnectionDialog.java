/*
 * Copyright 2013 Dart project authors.
 * 
 * Licensed under the Eclipse Public License v1.0 (the "License"); you may not use this file except
 * in compliance with the License. You may obtain a copy of the License at
 * 
 * http://www.eclipse.org/legal/epl-v10.html
 * 
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied. See the License for the specific language governing permissions and limitations under
 * the License.
 */

package com.google.dart.tools.debug.ui.internal.dialogs;

import com.google.dart.tools.debug.core.configs.DartServerLaunchConfigurationDelegate;
import com.google.dart.tools.debug.core.configs.DartiumLaunchConfigurationDelegate;
import com.google.dart.tools.debug.core.util.IRemoteConnectionDelegate;
import com.google.dart.tools.debug.core.webkit.ChromiumTabInfo;
import com.google.dart.tools.debug.core.webkit.DefaultChromiumTabChooser;
import com.google.dart.tools.debug.core.webkit.IChromiumTabChooser;
import com.google.dart.tools.debug.ui.internal.DartDebugUIPlugin;

import org.eclipse.core.runtime.CoreException;
import org.eclipse.core.runtime.IProgressMonitor;
import org.eclipse.core.runtime.IStatus;
import org.eclipse.core.runtime.Status;
import org.eclipse.core.runtime.jobs.Job;
import org.eclipse.jface.dialogs.ErrorDialog;
import org.eclipse.jface.dialogs.IDialogSettings;
import org.eclipse.jface.dialogs.TitleAreaDialog;
import org.eclipse.jface.layout.GridDataFactory;
import org.eclipse.jface.layout.GridLayoutFactory;
import org.eclipse.swt.SWT;
import org.eclipse.swt.events.SelectionAdapter;
import org.eclipse.swt.events.SelectionEvent;
import org.eclipse.swt.widgets.Combo;
import org.eclipse.swt.widgets.Composite;
import org.eclipse.swt.widgets.Control;
import org.eclipse.swt.widgets.Display;
import org.eclipse.swt.widgets.Group;
import org.eclipse.swt.widgets.Label;
import org.eclipse.swt.widgets.Shell;
import org.eclipse.swt.widgets.Text;
import org.eclipse.ui.IWorkbench;
import org.eclipse.ui.PlatformUI;

import java.util.List;

/**
 * A dialog to connect to remote debug instances.
 */
public class RemoteConnectionDialog extends TitleAreaDialog {

  /**
   * A class to choose a tab from the given list of tabs.
   */
  public static class ChromeTabChooser implements IChromiumTabChooser {
    public ChromeTabChooser() {

    }

    @Override
    public ChromiumTabInfo chooseTab(List<ChromiumTabInfo> tabs) {
      if (tabs.size() == 0) {
        return null;
      }

      if (tabs.size() == 1) {
        return tabs.get(0);
      }

      // TODO(devoncarew): display a chooser dialog

      return new DefaultChromiumTabChooser().chooseTab(tabs);
    }
  }

  public static class ConnectionJob extends Job {
    private IRemoteConnectionDelegate connectionDelegate;
    private String host;
    private int port;

    public ConnectionJob(IRemoteConnectionDelegate connectionDelegate, String host, int port) {
      super("Connecting...");

      this.connectionDelegate = connectionDelegate;
      this.host = host;
      this.port = port;
    }

    @Override
    protected IStatus run(IProgressMonitor monitor) {
      try {
        connectionDelegate.performRemoteConnection(host, port, monitor);
      } catch (CoreException ce) {
        displayError(ce);
      }

      return Status.OK_STATUS;
    }

    private void displayError(final CoreException exception) {
      Display.getDefault().asyncExec(new Runnable() {
        @Override
        public void run() {
          ErrorDialog.openError(
              PlatformUI.getWorkbench().getActiveWorkbenchWindow().getShell(),
              "Error Connecting to " + host + ":" + port,
              null,
              exception.getStatus());
        }
      });
    }
  }

  public enum ConnectionType {
    CHROME(
        "Chrome-based browser", //
        "Connect to a Chrome-based browser", //
        "To start Chrome with remote connections enabled, use the --remote-debugging-port=<port> command-line flag.", //
        "localhost", "9222"),

    COMMAND_LINE("Command-line VM", //
        "Connect to the stand-alone Dart VM", // 
        "To start the command-line VM in debug mode, use the --debug[:<port>] command-line flag.", //
        "localhost", "5858");

    String label;
    String message;
    String helpMessage;

    String hostDefault;
    String portDefault;

    ConnectionType(String label, String message, String helpMessage, String hostDefault,
        String portDefault) {
      this.label = label;
      this.message = message;
      this.helpMessage = helpMessage;
      this.hostDefault = hostDefault;
      this.portDefault = portDefault;
    }

    public void connection(String host, int port) {
      IRemoteConnectionDelegate connectionDelegate = null;

      switch (this) {
        case CHROME: {
          connectionDelegate = new DartiumLaunchConfigurationDelegate(new ChromeTabChooser());
          break;
        }

        case COMMAND_LINE: {
          connectionDelegate = new DartServerLaunchConfigurationDelegate();
          break;
        }

        default: {
          throw new IllegalArgumentException();
        }
      }

      if (connectionDelegate != null) {
        Job job = new ConnectionJob(connectionDelegate, host, port);
        job.schedule();
      }
    }
  }

  /**
   * Open an instance of a RemoteConnectionDialog.
   * 
   * @param workbench
   */
  public static void show(IWorkbench workbench) {
    RemoteConnectionDialog dialog = new RemoteConnectionDialog(
        workbench.getActiveWorkbenchWindow().getShell());

    dialog.open();
  }

  private Combo exceptionsCombo;

  private Text hostText;

  private Text portText;

  private Text instructionsLabel;

  /**
   * Create a new RemoteConnectionDialog with the given shell as its parent.
   * 
   * @param shell
   */
  public RemoteConnectionDialog(Shell shell) {
    super(shell);
  }

  @Override
  protected void configureShell(Shell newShell) {
    super.configureShell(newShell);

    newShell.setText("Open Remote Connection");
  }

  @Override
  protected Control createDialogArea(Composite parent) {
    Composite contents = (Composite) super.createDialogArea(parent);

    setTitle(getShell().getText());
    setTitleImage(DartDebugUIPlugin.getImage("wiz/run_wiz.png"));

    Composite composite = new Composite(contents, SWT.NONE);
    GridDataFactory.fillDefaults().grab(true, true).align(SWT.FILL, SWT.FILL).applyTo(composite);
    createDialogUI(composite);

    return contents;
  }

  @Override
  protected void okPressed() {
    ConnectionType connection = getConnectionType();

    String host = hostText.getText().trim();
    String port = portText.getText().trim();

    IDialogSettings settings = getDialogSettings();

    settings.put("selected", connection.ordinal());
    settings.put(connection.name() + ".host", host);
    settings.put(connection.name() + ".port", port);

    int connectionPort;

    try {
      connectionPort = Integer.parseInt(port);
    } catch (NumberFormatException nfe) {
      ErrorDialog.openError(getShell(), "Invalid Port", null, new Status(
          IStatus.ERROR,
          DartDebugUIPlugin.PLUGIN_ID,
          "\"" + port + "\" is an invalid port."));

      return;
    }

    connection.connection(host, connectionPort);

    super.okPressed();
  }

  private void createDialogUI(Composite parent) {
    GridLayoutFactory.fillDefaults().numColumns(2).margins(12, 6).applyTo(parent);

    Label label = new Label(parent, SWT.NONE);
    label.setText("Connect to:");

    exceptionsCombo = new Combo(parent, SWT.DROP_DOWN | SWT.READ_ONLY);
    exceptionsCombo.setItems(getConnectionLabels());
    exceptionsCombo.addSelectionListener(new SelectionAdapter() {
      @Override
      public void widgetSelected(SelectionEvent e) {
        handleComboChanged();
      }
    });

    // spacer
    label = new Label(parent, SWT.NONE);

    Group group = new Group(parent, SWT.NONE);
    group.setText("Connection parameters");
    GridDataFactory.fillDefaults().grab(true, false).applyTo(group);
    GridLayoutFactory.fillDefaults().numColumns(2).margins(12, 6).applyTo(group);

    label = new Label(group, SWT.NONE);
    label.setText("Host:");

    hostText = new Text(group, SWT.SINGLE | SWT.BORDER);
    GridDataFactory.fillDefaults().grab(true, false).applyTo(hostText);

    label = new Label(group, SWT.NONE);
    label.setText("Port:");

    portText = new Text(group, SWT.SINGLE | SWT.BORDER);
    GridDataFactory.fillDefaults().grab(true, false).applyTo(portText);

    // spacer
    label = new Label(parent, SWT.NONE);

    instructionsLabel = new Text(parent, SWT.WRAP | SWT.READ_ONLY);
    instructionsLabel.setBackground(parent.getBackground());
    GridDataFactory.fillDefaults().grab(true, false).hint(100, -1).applyTo(instructionsLabel);

    try {
      exceptionsCombo.select(getDialogSettings().getInt("selected"));
    } catch (NumberFormatException nfe) {
      exceptionsCombo.select(0);
    }

    handleComboChanged();
  }

  private String[] getConnectionLabels() {
    String[] labels = new String[ConnectionType.values().length];

    for (int i = 0; i < labels.length; i++) {
      labels[i] = ConnectionType.values()[i].label;
    }

    return labels;
  }

  private ConnectionType getConnectionType() {
    return ConnectionType.values()[exceptionsCombo.getSelectionIndex()];
  }

  private IDialogSettings getDialogSettings() {
    final String sectionName = "remoteConnectionSettings";

    IDialogSettings settings = DartDebugUIPlugin.getDefault().getDialogSettings();

    if (settings.getSection(sectionName) == null) {
      IDialogSettings section = settings.addNewSection(sectionName);

      for (ConnectionType connection : ConnectionType.values()) {
        section.put(connection.name() + ".host", connection.hostDefault);
        section.put(connection.name() + ".port", connection.portDefault);
      }
    }

    return settings.getSection(sectionName);
  }

  private void handleComboChanged() {
    ConnectionType connection = getConnectionType();

    setMessage(connection.message);
    instructionsLabel.setText(connection.helpMessage);

    IDialogSettings settings = getDialogSettings();
    hostText.setText(notNull(settings.get(connection.name() + ".host")));
    portText.setText(notNull(settings.get(connection.name() + ".port")));
  }

  private String notNull(String str) {
    return str == null ? "" : str;
  }

}
