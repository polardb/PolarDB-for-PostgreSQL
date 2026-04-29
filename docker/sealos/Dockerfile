FROM docker.1ms.run/openanolis/anolisos:8

COPY PolarDB-*.rpm /tmp/
COPY polardb-entrypoint.sh /usr/local/bin/polardb-entrypoint.sh

RUN rpm -ivh /tmp/PolarDB-*.rpm \
    && (id postgres >/dev/null 2>&1 || useradd -m postgres) \
    && mkdir -p /var/lib/polardb \
    && chown -R postgres:postgres /var/lib/polardb /u01/polardb_pg \
    && chmod +x /usr/local/bin/polardb-entrypoint.sh \
    && rm -f /tmp/PolarDB-*.rpm

ENV PATH=/u01/polardb_pg/bin:$PATH
ENV POLARDB_PORT=5432
ENV POLARDB_DATA_ROOT=/var/lib/polardb

EXPOSE 5432

ENTRYPOINT ["/usr/local/bin/polardb-entrypoint.sh"]
