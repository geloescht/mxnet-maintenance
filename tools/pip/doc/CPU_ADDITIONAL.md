<!--- Licensed to the Apache Software Foundation (ASF) under one -->
<!--- or more contributor license agreements.  See the NOTICE file -->
<!--- distributed with this work for additional information -->
<!--- regarding copyright ownership.  The ASF licenses this file -->
<!--- to you under the Apache License, Version 2.0 (the -->
<!--- "License"); you may not use this file except in compliance -->
<!--- with the License.  You may obtain a copy of the License at -->

<!---   http://www.apache.org/licenses/LICENSE-2.0 -->

<!--- Unless required by applicable law or agreed to in writing, -->
<!--- software distributed under the License is distributed on an -->
<!--- "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY -->
<!--- KIND, either express or implied.  See the License for the -->
<!--- specific language governing permissions and limitations -->
<!--- under the License. -->

Prerequisites
-------------
This package supports Linux, Mac OSX, and Windows platforms. You may also want to check:

- [mxnet-cu117](https://pypi.python.org/pypi/mxnet-cu117/) with CUDA-11.7 support.
- [mxnet-cu116](https://pypi.python.org/pypi/mxnet-cu116/) with CUDA-11.6 support.
- [mxnet-cu115](https://pypi.python.org/pypi/mxnet-cu115/) with CUDA-11.5 support.
- [mxnet-cu114](https://pypi.python.org/pypi/mxnet-cu114/) with CUDA-11.4 support.
- [mxnet-cu113](https://pypi.python.org/pypi/mxnet-cu113/) with CUDA-11.3 support.
- [mxnet-cu112](https://pypi.python.org/pypi/mxnet-cu112/) with CUDA-11.2 support.
- [mxnet-cu111](https://pypi.python.org/pypi/mxnet-cu111/) with CUDA-11.1 support.
- [mxnet-cu110](https://pypi.python.org/pypi/mxnet-cu110/) with CUDA-11.0 support.
- [mxnet-cu102](https://pypi.python.org/pypi/mxnet-cu102/) with CUDA-10.2 support.
- [mxnet-cu101](https://pypi.python.org/pypi/mxnet-cu101/) with CUDA-10.1 support.
- [mxnet-cu100](https://pypi.python.org/pypi/mxnet-cu100/) with CUDA-10.0 support.
- [mxnet-native](https://pypi.python.org/pypi/mxnet-native/) CPU variant without MKLDNN.

To use this package on Linux you need the `libquadmath.so.0` shared library. On
Debian based systems, including Ubuntu, run `sudo apt install libquadmath0` to
install the shared library. On RHEL based systems, including CentOS, run `sudo
yum install libquadmath` to install the shared library. As `libquadmath.so.0` is
a GPL library and MXNet part of the Apache Software Foundation, MXNet must not
redistribute `libquadmath.so.0` as part of the Pypi package and users must
manually install it.

To install for other platforms (e.g. Windows, Raspberry Pi/ARM) or other versions, check [Installing MXNet](https://mxnet.apache.org/versions/master) for instructions on building from source.

Installation
------------
To install, use:
```bash
pip install mxnet
```
