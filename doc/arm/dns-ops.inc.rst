.. Copyright (C) Internet Systems Consortium, Inc. ("ISC")
..
.. SPDX-License-Identifier: MPL-2.0
..
.. This Source Code Form is subject to the terms of the Mozilla Public
.. License, v. 2.0.  If a copy of the MPL was not distributed with this
.. file, you can obtain one at https://mozilla.org/MPL/2.0/.
..
.. See the COPYRIGHT file distributed with this work for additional
.. information regarding copyright ownership.

.. _ns_operations:

Name Server Operations
----------------------

.. _tools:

Tools for Use With the Name Server Daemon
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

This section describes several indispensable diagnostic, administrative, 
and monitoring tools available to the system administrator for
controlling and debugging the name server daemon.

.. _diagnostic_tools:

Diagnostic Tools
^^^^^^^^^^^^^^^^

The :iscman:`dig`, :iscman:`host`, and :iscman:`nslookup` programs are all command-line
tools for manually querying name servers. They differ in style and
output format.

:iscman:`dig`
   :iscman:`dig` is the most versatile and complete of these lookup tools. It
   has two modes: simple interactive mode for a single query, and batch
   mode, which executes a query for each in a list of several query
   lines. All query options are accessible from the command line.

   For more information and a list of available commands and options,
   see :ref:`man_dig`.

:iscman:`host`
   The :iscman:`host` utility emphasizes simplicity and ease of use. By
   default, it converts between host names and Internet addresses, but
   its functionality can be extended with the use of options.

   For more information and a list of available commands and options,
   see :ref:`man_host`.

:iscman:`nslookup`
   :iscman:`nslookup` has two modes: interactive and non-interactive.
   Interactive mode allows the user to query name servers for
   information about various hosts and domains, or to print a list of
   hosts in a domain. Non-interactive mode is used to print just the
   name and requested information for a host or domain.

   Due to its arcane user interface and frequently inconsistent
   behavior, we do not recommend the use of :iscman:`nslookup`. Use :iscman:`dig`
   instead.

.. _admin_tools:

Administrative Tools
^^^^^^^^^^^^^^^^^^^^

Administrative tools play an integral part in the management of a
server.

:iscman:`named-checkconf`
   The :iscman:`named-checkconf` program checks the syntax of a :iscman:`named.conf`
   file.

   For more information and a list of available commands and options,
   see :ref:`man_named-checkconf`.

:iscman:`named-checkzone`
   The :iscman:`named-checkzone` program checks a zone file for syntax and
   consistency.

   For more information and a list of available commands and options,
   see :ref:`man_named-checkzone`.

:iscman:`named-compilezone`
   This tool is similar to :iscman:`named-checkzone` but it always dumps the zone content
   to a specified file (typically in a different format).

   For more information and a list of available commands and options,
   see :ref:`man_named-compilezone`.

.. _ops_rndc:

:iscman:`rndc`
   The remote name daemon control (:iscman:`rndc`) program allows the system
   administrator to control the operation of a name server.

   See :ref:`man_rndc` for details of the available :iscman:`rndc`
   commands.

   :iscman:`rndc` requires a configuration file, since all communication with
   the server is authenticated with digital signatures that rely on a
   shared secret, and there is no way to provide that secret other than
   with a configuration file. The default location for the :iscman:`rndc`
   configuration file is |rndc_conf|, but an alternate location
   can be specified with the :option:`-c <rndc -c>` option. If the configuration file is
   not found, :iscman:`rndc` also looks in |rndc_key| (or whatever
   ``sysconfdir`` was defined when the BIND build was configured). The
   ``rndc.key`` file is generated by running :option:`rndc-confgen -a` as
   described in :ref:`controls_statement_definition_and_usage`.

   The format of the configuration file is similar to that of
   :iscman:`named.conf`, but is limited to only four statements: the ``options``,
   ``key``, ``server``, and ``include`` statements. These statements are
   what associate the secret keys to the servers with which they are
   meant to be shared. The order of statements is not significant.

   The ``options`` statement has three clauses: ``default-server``,
   ``default-key``, and ``default-port``. ``default-server`` takes a
   host name or address argument and represents the server that is
   contacted if no :option:`-s <rndc -s>` option is provided on the command line.
   ``default-key`` takes the name of a key as its argument, as defined
   by a ``key`` statement. ``default-port`` specifies the port to which
   :iscman:`rndc` should connect if no port is given on the command line or in
   a ``server`` statement.

   The ``key`` statement defines a key to be used by :iscman:`rndc` when
   authenticating with :iscman:`named`. Its syntax is identical to the ``key``
   statement in :iscman:`named.conf`. The keyword ``key`` is followed by a key
   name, which must be a valid domain name, though it need not actually
   be hierarchical; thus, a string like ``rndc_key`` is a valid name.
   The ``key`` statement has two clauses: ``algorithm`` and ``secret``.
   While the configuration parser accepts any string as the argument
   to ``algorithm``, currently only the strings ``hmac-md5``,
   ``hmac-sha1``, ``hmac-sha224``, ``hmac-sha256``,
   ``hmac-sha384``, and ``hmac-sha512`` have any meaning. The secret
   is a Base64-encoded string as specified in :rfc:`3548`.

   The ``server`` statement associates a key defined using the ``key``
   statement with a server. The keyword ``server`` is followed by a host
   name or address. The ``server`` statement has two clauses: ``key``
   and ``port``. The ``key`` clause specifies the name of the key to be
   used when communicating with this server, and the ``port`` clause can
   be used to specify the port :iscman:`rndc` should connect to on the server.

   A sample minimal configuration file is as follows:

   ::

      key rndc_key {
           algorithm "hmac-sha256";
           secret
             "c3Ryb25nIGVub3VnaCBmb3IgYSBtYW4gYnV0IG1hZGUgZm9yIGEgd29tYW4K";
      };
      options {
           default-server 127.0.0.1;
           default-key    rndc_key;
      };

   This file, if installed as |rndc_conf|, allows the
   command:

   :option:`rndc reload`

   to connect to 127.0.0.1 port 953 and causes the name server to reload,
   if a name server on the local machine is running with the following
   controls statements:

   ::

      controls {
          inet 127.0.0.1
              allow { localhost; } keys { rndc_key; };
      };

   and it has an identical key statement for ``rndc_key``.

   Running the :iscman:`rndc-confgen` program conveniently creates an
   :iscman:`rndc.conf` file, and also displays the corresponding
   ``controls`` statement needed to add to :iscman:`named.conf`.
   Alternatively, it is possible to run :option:`rndc-confgen -a` to set up an
   ``rndc.key`` file and not modify :iscman:`named.conf` at all.

Signals
~~~~~~~

Certain Unix signals cause the name server to take specific actions, as
described in the following table. These signals can be sent using the
``kill`` command.

+--------------+-------------------------------------------------------------+
| ``SIGHUP``   | Causes the server to read :iscman:`named.conf` and reload   |
|              | the database.                                               |
+--------------+-------------------------------------------------------------+
| ``SIGTERM``  | Causes the server to clean up and exit.                     |
+--------------+-------------------------------------------------------------+
| ``SIGINT``   | Causes the server to clean up and exit.                     |
+--------------+-------------------------------------------------------------+

