FROM inspircd:testing
USER root
RUN apk add --no-cache python3
COPY fake-ssh.py /usr/bin/ssh
COPY integration.py /test.py
RUN chmod 755 /usr/bin/ssh
USER inspircd
ENTRYPOINT ["python3", "/test.py"]
