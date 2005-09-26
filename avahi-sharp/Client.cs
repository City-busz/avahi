/* $Id$ */

/***
  This file is part of avahi.

  avahi is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  avahi is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General
  Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with avahi; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/


using System;
using System.Threading;
using System.Collections;
using System.Runtime.InteropServices;

namespace Avahi
{
    internal enum ResolverEvent {
        Found,
        Timeout,
        NotFound,
        Failure
    }
    
    internal enum BrowserEvent {
        Added,
        Removed,
        CacheExhausted,
        AllForNow,
        NotFound,
        Failure
    }

    internal delegate int PollCallback (IntPtr ufds, uint nfds, int timeout);
    internal delegate void ClientCallback (IntPtr client, ClientState state, IntPtr userData);

    public delegate void ClientStateHandler (object o, ClientState state);

    public enum Protocol {
        Unspecified = -1,
        IPv4 = 0,
        IPv6 = 1
    }
    
    public enum ClientState {
        Invalid,
        Registering,
        Running,
        Collision,
        Disconnected = 100
    }

    [Flags]
    public enum LookupFlags {
        None = 0,
        UseWideArea = 1,
        UseMulticast = 2,
        NoTxt = 4,
        NoAddress = 8
    }

    [Flags]
    public enum LookupResultFlags {
        None = 0,
        Cached = 1,
        WideArea = 2,
        Multicast = 4
    }
    
    public class Client : IDisposable
    {
        private IntPtr handle;
        private ClientCallback cb;
        private PollCallback pollcb;
        private IntPtr spoll;

        private Thread thread;

        [DllImport ("avahi-client")]
        private static extern IntPtr avahi_client_new (IntPtr poll, ClientCallback handler,
                                                       IntPtr userData, out int error);

        [DllImport ("avahi-client")]
        private static extern void avahi_client_free (IntPtr handle);

        [DllImport ("avahi-client")]
        private static extern IntPtr avahi_client_get_version_string (IntPtr handle);

        [DllImport ("avahi-client")]
        private static extern IntPtr avahi_client_get_host_name (IntPtr handle);

        [DllImport ("avahi-client")]
        private static extern IntPtr avahi_client_get_domain_name (IntPtr handle);

        [DllImport ("avahi-client")]
        private static extern IntPtr avahi_client_get_host_name_fqdn (IntPtr handle);

        [DllImport ("avahi-client")]
        private static extern ClientState avahi_client_get_state (IntPtr handle);

        [DllImport ("avahi-client")]
        private static extern int avahi_client_errno (IntPtr handle);
        
        [DllImport ("avahi-common")]
        private static extern IntPtr avahi_simple_poll_new ();

        [DllImport ("avahi-common")]
        private static extern IntPtr avahi_simple_poll_get (IntPtr spoll);

        [DllImport ("avahi-common")]
        private static extern void avahi_simple_poll_free (IntPtr spoll);

        [DllImport ("avahi-common")]
        private static extern int avahi_simple_poll_iterate (IntPtr spoll, int timeout);

        [DllImport ("avahi-common")]
        private static extern void avahi_simple_poll_set_func (IntPtr spoll, PollCallback cb);

        [DllImport ("avahi-common")]
        private static extern void avahi_simple_poll_quit (IntPtr spoll);

        [DllImport ("avahi-client")]
        private static extern uint avahi_client_get_local_service_cookie (IntPtr client);

        [DllImport ("avahi-client")]
        private static extern int avahi_client_is_service_local (IntPtr client, int iface, Protocol proto,
                                                                 IntPtr name, IntPtr type, IntPtr domain);


        [DllImport ("libc")]
        private static extern int poll(IntPtr ufds, uint nfds, int timeout);

        public event ClientStateHandler StateChanged;

        internal IntPtr Handle
        {
            get { return handle; }
        }
        
        public string Version
        {
            get {
                lock (this) {
                    return Utility.PtrToString (avahi_client_get_version_string (handle));
                }
            }
        }

        public string HostName
        {
            get {
                lock (this) {
                    return Utility.PtrToString (avahi_client_get_host_name (handle));
                }
            }
        }

        public string DomainName
        {
            get {
                lock (this) {
                    return Utility.PtrToString (avahi_client_get_domain_name (handle));
                }
            }
        }

        public string HostNameFqdn
        {
            get {
                lock (this) {
                    return Utility.PtrToString (avahi_client_get_host_name_fqdn (handle));
                }
            }
        }

        public ClientState State
        {
            get {
                lock (this) {
                    return (ClientState) avahi_client_get_state (handle);
                }
            }
        }

        public uint LocalServiceCookie
        {
            get {
                lock (this) {
                    return avahi_client_get_local_service_cookie (handle);
                }
            }
        }

        internal int LastError
        {
            get {
                lock (this) {
                    return avahi_client_errno (handle);
                }
            }
        }

        public Client ()
        {
            spoll = avahi_simple_poll_new ();

            pollcb = OnPollCallback;
            avahi_simple_poll_set_func (spoll, pollcb);
            IntPtr poll = avahi_simple_poll_get (spoll);
            cb = OnClientCallback;

            int error;
            handle = avahi_client_new (poll, cb, IntPtr.Zero, out error);
            if (error != 0)
                throw new ClientException (error);

            thread = new Thread (PollLoop);
            thread.IsBackground = true;
            thread.Start ();
        }

        ~Client ()
        {
            Dispose ();
        }

        public void Dispose ()
        {
            lock (this) {
                if (handle != IntPtr.Zero) {
                    thread.Abort ();

                    avahi_client_free (handle);
                    avahi_simple_poll_quit (spoll);
                    avahi_simple_poll_free (spoll);
                    handle = IntPtr.Zero;
                }
            }
        }

        public bool IsServiceLocal (ServiceInfo service)
        {
            return IsServiceLocal (service.NetworkInterface, service.Protocol, service.Name,
                                   service.ServiceType, service.Domain);
        }

        public bool IsServiceLocal (int iface, Protocol proto, string name, string type, string domain)
        {
            IntPtr namePtr = Utility.StringToPtr (name);
            IntPtr typePtr = Utility.StringToPtr (type);
            IntPtr domainPtr = Utility.StringToPtr (domain);
            
            int result = avahi_client_is_service_local (handle, iface, proto, namePtr, typePtr, domainPtr);

            Utility.Free (namePtr);
            Utility.Free (typePtr);
            Utility.Free (domainPtr);

            return result == 1;
        }

        internal void CheckError ()
        {
            int error = LastError;

            if (error != 0)
                throw new ClientException (error);
        }
        
        private void OnClientCallback (IntPtr client, ClientState state, IntPtr userData)
        {
            if (StateChanged != null)
                StateChanged (this, state);
        }

        private int OnPollCallback (IntPtr ufds, uint nfds, int timeout) {
            Monitor.Exit (this);
            int result = poll (ufds, nfds, timeout);
            Monitor.Enter (this);
            return result;
        }

        private void PollLoop () {
            try {
                lock (this) {
                    while (true) {
                        if (avahi_simple_poll_iterate (spoll, -1) != 0)
                            break;
                    }
                }
            } catch (ThreadAbortException e) {
            } catch (Exception e) {
                Console.Error.WriteLine ("Error in avahi-sharp event loop: " + e);
            }
        }
    }
}
