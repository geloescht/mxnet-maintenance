;;
;; Licensed to the Apache Software Foundation (ASF) under one or more
;; contributor license agreements.  See the NOTICE file distributed with
;; this work for additional information regarding copyright ownership.
;; The ASF licenses this file to You under the Apache License, Version 2.0
;; (the "License"); you may not use this file except in compliance with
;; the License.  You may obtain a copy of the License at
;;
;;    http://www.apache.org/licenses/LICENSE-2.0
;;
;; Unless required by applicable law or agreed to in writing, software
;; distributed under the License is distributed on an "AS IS" BASIS,
;; WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
;; See the License for the specific language governing permissions and
;; limitations under the License.
;;

(defproject objectdetector "0.1.0-SNAPSHOT"
  :description "Object detection using infer with MXNet"
  :repositories [["vendredi" "https://repository.hellonico.info/repository/hellonico/"]]
  :plugins [[lein-cljfmt "0.5.7"]]
  :aliases {"run-detector" ["run" "--" "-m" "models/resnet50_ssd/resnet50_ssd_model" "-i" "images/dog.jpg" "-d" "images/"]}
  :dependencies [[org.clojure/clojure "1.9.0"]
                 [org.clojure/tools.cli "0.4.1"]
                 [org.apache.mxnet.contrib.clojure/clojure-mxnet "1.9.0-SNAPSHOT"]]
  :main ^:skip-aot infer.objectdetector-example
  :profiles {:uberjar {:aot :all}})
