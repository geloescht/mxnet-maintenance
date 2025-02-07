#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

"""Multi arch dockerized build tool.

"""

__author__ = 'Marco de Abreu, Kellen Sunderland, Anton Chernov, Pedro Larroy'
__version__ = '0.3'

import argparse
import glob
import hashlib
import os
import pprint
import re
import shutil
import signal
import subprocess
from platform import machine
from itertools import chain
from subprocess import check_call, check_output
from typing import *

import yaml

from util import *

# keywords to identify arm-based dockerfiles
AARCH_FILE_KEYWORDS = ['aarch64', 'armv']

def get_dockerfiles_path():
    return "docker"

def get_docker_compose_platforms(path: str = get_dockerfiles_path()):
    platforms = set()
    with open(os.path.join(path, "docker-compose.yml"), "r") as f:
        compose_config = yaml.load(f.read(), yaml.SafeLoader)
        for platform in compose_config["services"]:
            platforms.add(platform)
    return platforms

def get_docker_compose_dockerfiles(path: str = get_dockerfiles_path()):
    dockerfiles = set()
    with open(os.path.join(path, "docker-compose.yml"), "r") as f:
        compose_config = yaml.load(f.read(), yaml.SafeLoader)
        for platform in compose_config["services"]:
            dockerfiles.add("docker/" + compose_config['services'][platform]['build']['dockerfile'])
    return dockerfiles

def get_platforms(path: str = get_dockerfiles_path(), arch=machine()) -> List[str]:
    """Get a list of platforms given our dockerfiles"""
    dockerfiles = glob.glob(os.path.join(path, "Dockerfile.*"))
    dockerfiles = set(filter(lambda x: x[-1] != '~', dockerfiles))
    dockerfiles = dockerfiles - get_docker_compose_dockerfiles()
    files = set(map(lambda x: re.sub(r"Dockerfile.(.*)", r"\1", x), dockerfiles))
    files.update(["build."+x for x in get_docker_compose_platforms()])
    arm_files = set(filter(lambda x: any(y in x for y in AARCH_FILE_KEYWORDS), files))
    if arch == 'x86_64':
        files = files - arm_files
    elif arch == 'aarch64':
        files = arm_files
    platforms = list(map(lambda x: os.path.split(x)[1], sorted(files)))
    return platforms

def _find_copied_files(dockerfile):
    """
    Creates a list of files copied into given dockerfile.
    """
    copied_files = []
    basedir = os.path.dirname(dockerfile)
    with open(dockerfile, "r") as f:
        for line in f.readlines():
            if line.startswith("COPY "):
                copied_files.append(os.path.join(basedir, line.split(" ")[1]))
    return copied_files

def _hash_file(ctx, filename):
    """
    Add contents of passed file into passed hash context.
    """
    bufsiz = 16384
    with open(filename,"rb") as f:
        while True:
            d = f.read(bufsiz)
            if not d:
                break
            ctx.update(d)

def is_docker_compose(platform: str) -> bool:
    """:return: boolean whether specified platform container uses docker-compose"""
    platlist = get_docker_compose_platforms()
    platform = platform.split(".")[1] if any(x in platform for x in ['build.', 'publish.']) else platform
    return platform in platlist


def get_docker_tag(platform: str, registry: str) -> str:
    """:return: docker tag to be used for the container"""
    platform = platform if any(x in platform for x in ['build.', 'publish.']) else 'build.{}'.format(platform)
    if is_docker_compose(platform):
        with open("docker/docker-compose.yml", "r") as f:
            compose_config = yaml.load(f.read(), yaml.SafeLoader)
            return compose_config["services"][platform.split(".")[1]]["image"].replace('${DOCKER_CACHE_REGISTRY}', registry)

    if not registry:
        registry = "mxnet_local"
    dockerfile = get_dockerfile(platform)
    sha256 = hashlib.sha256()
    _hash_file(sha256, dockerfile)
    for f in _find_copied_files(dockerfile):
        _hash_file(sha256, f)
    return "{0}:{1}-{2}".format(registry, platform, sha256.hexdigest()[:12])


def get_dockerfile(platform: str, path=get_dockerfiles_path()) -> str:
    platform = platform if any(x in platform for x in ['build.', 'publish.']) else 'build.{}'.format(platform)
    return os.path.join(path, "Dockerfile.{0}".format(platform))


def build_docker(platform: str, registry: str, num_retries: int, no_cache: bool,
                 cache_intermediate: bool=False) -> str:
    """
    Build a container for the given platform
    :param platform: Platform
    :param registry: Dockerhub registry name
    :param num_retries: Number of retries to build the docker image
    :param no_cache: pass no-cache to docker to rebuild the images
    :return: Id of the top level image
    """
    tag = get_docker_tag(platform=platform, registry=registry)
    # docker-compose
    if is_docker_compose(platform):
        docker_compose_platform = platform.split(".")[1] if any(x in platform for x in ['build.', 'publish.']) else platform
        logging.info('Building docker container tagged \'%s\' based on ci/docker/docker-compose.yml', tag)
        # We add a user with the same group as the executing non-root user so files created in the
        # container match permissions of the local user. Same for the group.
        cmd = ['docker-compose', '-f', 'docker/docker-compose.yml', 'build',
               "--build-arg", "USER_ID={}".format(os.getuid()),
               "--build-arg", "GROUP_ID={}".format(os.getgid())]
        if cache_intermediate:
            cmd.append('--no-rm')
        cmd.append(docker_compose_platform)
    else:
        logging.info("Building docker container tagged '%s'", tag)
        #
        # We add a user with the same group as the executing non-root user so files created in the
        # container match permissions of the local user. Same for the group.
        #
        # These variables are used in the docker files to create user and group with these ids.
        # see: docker/install/ubuntu_adduser.sh
        #
        # cache-from is needed so we use the cached images tagged from the remote via
        # docker pull see: docker_cache.load_docker_cache
        #
        # This also prevents using local layers for caching: https://github.com/moby/moby/issues/33002
        # So to use local caching, we should omit the cache-from by using --no-dockerhub-cache argument to this
        # script.
        #
        # This doesn't work with multi head docker files.
        #
        cmd = ["docker", "build",
            "-f", get_dockerfile(platform),
            "--build-arg", "USER_ID={}".format(os.getuid()),
            "--build-arg", "GROUP_ID={}".format(os.getgid())]
        if no_cache:
            cmd.append("--no-cache")
        if cache_intermediate:
            cmd.append("--rm=false")
        elif registry:
            cmd.extend(["--cache-from", tag])
        cmd.extend(["-t", tag, get_dockerfiles_path()])

    env = os.environ.copy()
    env["DOCKER_CACHE_REGISTRY"] = registry

    @retry(subprocess.CalledProcessError, tries=num_retries)
    def run_cmd(c, e):
        logging.info("Running command: '%s'", ' '.join(c))
        check_call(c, env=e)

    run_cmd(cmd, env)

    # Get image id by reading the tag. It's guaranteed (except race condition) that the tag exists. Otherwise, the
    # check_call would have failed
    image_id = _get_local_image_id(docker_tag=tag)
    if not image_id:
        raise FileNotFoundError('Unable to find docker image id matching with {}'.format(tag))
    return image_id


def _get_local_image_id(docker_tag):
    """
    Get the image id of the local docker layer with the passed tag
    :param docker_tag: docker tag
    :return: Image id as string or None if tag does not exist
    """
    cmd = ["docker", "images", "-q", docker_tag]
    image_id_b = check_output(cmd)
    image_id = image_id_b.decode('utf-8').strip()
    if not image_id:
        raise RuntimeError('Unable to find docker image id matching with tag {}'.format(docker_tag))
    return image_id


def buildir() -> str:
    return os.path.join(get_mxnet_root(), "build")


def default_ccache_dir() -> str:
    """:return: ccache directory for the current platform"""
    # Share ccache across containers
    if 'CCACHE_DIR' in os.environ:
        ccache_dir = os.path.realpath(os.environ['CCACHE_DIR'])
        try:
            os.makedirs(ccache_dir, exist_ok=True)
            return ccache_dir
        except PermissionError:
            logging.info('Unable to make dirs at %s, falling back to local temp dir', ccache_dir)
    # In osx tmpdir is not mountable by default
    import platform
    if platform.system() == 'Darwin':
        ccache_dir = "/tmp/_mxnet_ccache"
        os.makedirs(ccache_dir, exist_ok=True)
        return ccache_dir
    return os.path.join(os.path.expanduser("~"), ".ccache")


def container_run(platform: str,
                  nvidia_runtime: bool,
                  docker_registry: str,
                  shared_memory_size: str,
                  local_ccache_dir: str,
                  command: List[str],
                  environment: Dict[str, str],
                  dry_run: bool = False) -> int:
    """Run command in a container"""
    # set default environment variables
    environment.update({
        'CCACHE_MAXSIZE': '500G',
        'CCACHE_TEMPDIR': '/tmp/ccache',  # temp dir should be local and not shared
        'CCACHE_DIR': '/work/ccache',  # this path is inside the container as /work/ccache is mounted
        'CCACHE_LOGFILE': '/tmp/ccache.log',  # a container-scoped log, useful for ccache verification.
    })
    environment.update({k: os.environ[k] for k in ['CCACHE_MAXSIZE'] if k in os.environ})

    tag = get_docker_tag(platform=platform, registry=docker_registry)
    mx_root = get_mxnet_root()
    local_build_folder = buildir()
    # We need to create it first, otherwise it will be created by the docker daemon with root only permissions
    os.makedirs(local_build_folder, exist_ok=True)
    os.makedirs(local_ccache_dir, exist_ok=True)
    logging.info("Using ccache directory: %s", local_ccache_dir)

    # Build docker command
    docker_arg_list = [
        "--cap-add", "SYS_PTRACE", # Required by ASAN
        '--rm',
        '--shm-size={}'.format(shared_memory_size),
        # mount mxnet root
        '-v', "{}:/work/mxnet".format(mx_root),
        # mount mxnet/build for storing build
        '-v', "{}:/work/build".format(local_build_folder),
        '-v', "{}:/work/ccache".format(local_ccache_dir),
        '-u', '{}:{}'.format(os.getuid(), os.getgid())
    ]
    for e in environment.keys():
        docker_arg_list += ['-e', '{}={}'.format(e, environment[e])]
    docker_arg_list += [tag]
    docker_arg_list.extend(command)

    def docker_run_cmd(cmd):
        logging.info("Running %s in container %s", command, tag)
        logging.info("Executing command:\n%s\n", ' \\\n\t'.join(cmd))
        subprocess.run(cmd, stdout=sys.stdout, stderr=sys.stderr, check=True)

    if not dry_run:
        if not nvidia_runtime:
            docker_run_cmd(['docker', 'run'] + docker_arg_list)
        else:
            try:
                docker_run_cmd(['docker', 'run', '--gpus', 'all'] + docker_arg_list)
            except subprocess.CalledProcessError as e:
                if e.returncode == 125:
                    docker_run_cmd(['docker', 'run', '--runtime', 'nvidia'] + docker_arg_list)
                else:
                    raise

    return 0


def list_platforms(arch=machine()) -> str:
    return "\nSupported platforms:\n{}".format('\n'.join(get_platforms(arch=arch)))


def load_docker_cache(platform, tag, docker_registry) -> None:
    """Imports tagged container from the given docker registry"""
    if docker_registry:
        env = os.environ.copy()
        env["DOCKER_CACHE_REGISTRY"] = docker_registry
        if is_docker_compose(platform):
            docker_compose_platform = platform.split(".")[1] if any(x in platform for x in ['build.', 'publish.']) else platform
            if "dkr.ecr" in docker_registry:
                try:
                    import docker_cache
                    docker_cache._ecr_login(docker_registry)
                except Exception:
                    logging.exception('Unable to login to ECR...')
            cmd = ['docker-compose', '-f', 'docker/docker-compose.yml', 'pull', '--quiet', docker_compose_platform]
            logging.info("Running command: '%s'", ' '.join(cmd))
            check_call(cmd, env=env)
            return

        # noinspection PyBroadException
        try:
            import docker_cache
            logging.info('Docker cache download is enabled from registry %s', docker_registry)
            docker_cache.load_docker_cache(registry=docker_registry, docker_tag=tag)
        except Exception:
            logging.exception('Unable to retrieve Docker cache. Continue without...')
    else:
        logging.info('Distributed docker cache disabled')


def log_environment():
    instance_info = ec2_instance_info()
    if instance_info:
        logging.info("EC2: %s", instance_info)
    pp = pprint.PrettyPrinter(indent=4)
    logging.debug("Build environment: %s", pp.pformat(dict(os.environ)))


def main() -> int:
    config_logging()

    logging.info("MXNet container based build tool.")
    log_environment()
    chdir_to_script_directory()

    parser = argparse.ArgumentParser(description="""Utility for building and testing MXNet on docker
    containers""", epilog="")
    parser.add_argument("-p", "--platform",
                        help="platform",
                        type=str)

    parser.add_argument("-A", "--architecture",
                        help="Architecture of images to build (x86_64 or aarch64). Default is current machine type.",
                        default=machine(),
                        dest='architecture')

    parser.add_argument("-b", "--build-only",
                        help="Only build the container, don't build the project",
                        action='store_true')

    parser.add_argument("-R", "--run-only",
                        help="Only run the container, don't rebuild the container",
                        action='store_true')

    parser.add_argument("-a", "--all",
                        help="build for all platforms",
                        action='store_true')

    parser.add_argument("-n", "--nvidiadocker",
                        help="Use nvidia docker",
                        action='store_true')

    parser.add_argument("--shm-size",
                        help="Size of the shared memory /dev/shm allocated in the container (e.g '1g')",
                        default='500m',
                        dest="shared_memory_size")

    parser.add_argument("-l", "--list",
                        help="List platforms",
                        action='store_true')

    parser.add_argument("--print-docker-run",
                        help="print docker run command for manual inspection",
                        action='store_true')

    parser.add_argument("-d", "--docker-registry",
                        help="Dockerhub registry name to retrieve cache from.",
                        default='mxnetci',
                        type=str)

    parser.add_argument("-r", "--docker-build-retries",
                        help="Number of times to retry building the docker image. Default is 1",
                        default=1,
                        type=int)

    parser.add_argument("--no-cache", action="store_true",
                        help="passes --no-cache to docker build")

    parser.add_argument("--cache-intermediate", action="store_true",
                        help="passes --rm=false to docker build")

    parser.add_argument("-e", "--environment", nargs="*", default=[],
                        help="Environment variables for the docker container. "
                        "Specify with a list containing either names or name=value")

    parser.add_argument("command",
                        help="command to run in the container",
                        nargs='*', action='append', type=str)

    parser.add_argument("--ccache-dir",
                        default=default_ccache_dir(),
                        help="ccache directory",
                        type=str)

    args = parser.parse_args()

    command = list(chain(*args.command))

    environment = dict([(e.split('=')[:2] if '=' in e else (e, os.environ[e]))
                        for e in args.environment])

    if args.list:
        print(list_platforms(arch=args.architecture))
    elif args.platform:
        platform = args.platform
        tag = get_docker_tag(platform=platform, registry=args.docker_registry)
        if args.docker_registry:   
            load_docker_cache(platform=platform, tag=tag, docker_registry=args.docker_registry)
        if not args.run_only:
            build_docker(platform=platform, registry=args.docker_registry,
                         num_retries=args.docker_build_retries, no_cache=args.no_cache,
                         cache_intermediate=args.cache_intermediate)
        else:
            logging.info("Skipping docker build step.")

        if args.build_only:
            logging.warning("Container was just built. Exiting due to build-only.")
            return 0

        # noinspection PyUnusedLocal
        ret = 0
        if command:
            ret = container_run(
                platform=platform, nvidia_runtime=args.nvidiadocker,
                shared_memory_size=args.shared_memory_size, command=command, docker_registry=args.docker_registry,
                local_ccache_dir=args.ccache_dir, environment=environment)
        elif args.print_docker_run:
            command = []
            ret = container_run(
                platform=platform, nvidia_runtime=args.nvidiadocker,
                shared_memory_size=args.shared_memory_size, command=command, docker_registry=args.docker_registry,
                local_ccache_dir=args.ccache_dir, dry_run=True, environment=environment)
        else:
            # With no commands, execute a build function for the target platform
            command = ["/work/mxnet/ci/docker/runtime_functions.sh", "build_{}".format(platform)]
            logging.info("No command specified, trying default build: %s", ' '.join(command))
            ret = container_run(
                platform=platform, nvidia_runtime=args.nvidiadocker,
                shared_memory_size=args.shared_memory_size, command=command, docker_registry=args.docker_registry,
                local_ccache_dir=args.ccache_dir, environment=environment)

        if ret != 0:
            logging.critical("Execution of %s failed with status: %d", command, ret)
            return ret

    elif args.all:
        platforms = get_platforms(arch=args.architecture)
        platforms = [platform for platform in platforms if 'build.' in platform]
        logging.info("Building for all platforms: %s", platforms)
        logging.info("Artifacts will be produced in the build/ directory.")
        for platform in platforms:
            tag = get_docker_tag(platform=platform, registry=args.docker_registry)
            load_docker_cache(platform=platform, tag=tag, docker_registry=args.docker_registry)
            build_docker(platform, registry=args.docker_registry,
                         num_retries=args.docker_build_retries, no_cache=args.no_cache,
                         cache_intermediate=args.cache_intermediate)
            if args.build_only:
                continue
            shutil.rmtree(buildir(), ignore_errors=True)
            build_platform = "build_{}".format(platform)
            plat_buildir = os.path.abspath(os.path.join(get_mxnet_root(), '..',
                                                        "mxnet_{}".format(build_platform)))
            if os.path.exists(plat_buildir):
                logging.warning("%s already exists, skipping", plat_buildir)
                continue
            command = ["/work/mxnet/ci/docker/runtime_functions.sh", build_platform]
            container_run(
                platform=platform, nvidia_runtime=args.nvidiadocker,
                shared_memory_size=args.shared_memory_size, command=command, docker_registry=args.docker_registry,
                local_ccache_dir=args.ccache_dir, environment=environment)
            shutil.move(buildir(), plat_buildir)
            logging.info("Built files left in: %s", plat_buildir)

    else:
        parser.print_help()
        list_platforms(arch=args.architecture)
        print("""
Examples:

./build.py -p armv7

    Will build a docker container with cross compilation tools and build MXNet for armv7 by
    running: ci/docker/runtime_functions.sh build_armv7 inside the container.

./build.py -p armv7 ls

    Will execute the given command inside the armv7 container

./build.py -p armv7 --print-docker-run

    Will print a docker run command to get inside the container in a shell

./build.py -a

    Builds for all platforms and leaves artifacts in build_<platform>

    """)

    return 0


if __name__ == '__main__':
    sys.exit(main())
