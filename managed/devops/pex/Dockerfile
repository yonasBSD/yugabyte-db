# Get the latest docker image
FROM quay.io/pypa/manylinux_2_28_x86_64

# Perform general yum updates
RUN yum --enablerepo=extras -y install epel-release python3-pip
RUN pip3 install --upgrade pip

# Dependency section can be configured every time a
# new Python module is added to Yugabyte Platform
# (if it requires third-party shared dependencies)

# Install psycopg2==2.9.1 third-party shared dependencies
RUN yum -y install postgresql-devel python3-devel gcc libcom_err.x86_64

# Install python-ldap==3.3.1 third-party shared dependencies
RUN yum -y install openldap-devel

# Install pex, wheel, and auditwheel (for constructing and
# repairing the generated PEX file to ensure compatibility
# across all python versions, and
# for Linux platform and cross-platform compatibility).
RUN pip3 install pex==2.45.3 wheel auditwheel

# Copying the testPex folder, which contains the build_pex.sh
# script we want to run.
RUN mkdir /code
COPY . /code/pex
WORKDIR /code/pex/
ENV PEX_ROOT=/code/pex/.cache
ENTRYPOINT [ "/code/pex/build_pex.sh" ]
