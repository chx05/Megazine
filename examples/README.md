This folder provides an example for each case of use of this library.

In order of triviality:
* db_as_a_config: Shows how to use the library to store app settings, general configurations, and other "startup initialisation" data which can be customised at the application level. In short, the typical use case for Json: a configuration file. It is the most trivial because the only thing the lib has to do is load data from a local file and write it to your struct instances, as far as reading is concerned. Even when it comes to writing (from struct instances to the local file), the situation is trivial: the lib takes the data from your struct instances and writes it directly to the file, without any special precautions. Suitable for single threaded config file read and write. These kind of interactions with the db are usually blocking for your program.

* local_db: Shows how to use the library to synchronize ram and disk at any frequency, and from more than one thread simultaneously. Suitable for saving game data, program checkpoints, or even e-commerce orders, anything as long as the database is managed by a single process. These kind of interactions with the db are usually non-blocking for your program.

* db_as_a_server: Shows how to securely expose any other model cited in this document over the network.

* local_concurrent_db: Shows how to use a local database concurrently with other processes (concurrency with multiple threads is already handled by the local_db example).

* acid_db: Shows how to use any other model cited in this document in a secure and reliable way to prevent database corruption and always be able to recover data in case of sudden interruption. This is essential for applications where losing any data is unacceptable.

* db_replication: Shows how to easily replicate the database over a network of other machines (maybe physically distant, to protect your most important data from physical problems).


For a general good use of this library, I suggest to separate your data in well-distinct databases. Use one acid db for your critical data, one for checkpoints, game data and related, and a third one for config files.

You may even separate more. There is no limit to the number of databases the library can handle at the same time.