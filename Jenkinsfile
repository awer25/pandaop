pipeline {
  agent any
  environment {
    DOCKER_IMAGE_TAG = "panda:build-${env.GIT_COMMIT}"
  }
  stages {
    stage('Build Docker Image') {
      steps {
        timeout(time: 60, unit: 'MINUTES') {
          script {
            sh 'git archive -v -o panda.tar.gz --format=tar.gz HEAD'
            dockerImage = docker.build("${env.DOCKER_IMAGE_TAG}")
          }
        }
      }
    }
    stage('PEDAL tests') {
      steps {
        lock(resource: "pandas", inversePrecedence: true, quantity: 1) {
          timeout(time: 20, unit: 'MINUTES') {
            script {
              sh "docker run --rm --privileged \
                    --volume /dev/bus/usb:/dev/bus/usb \
                    --volume /var/run/dbus:/var/run/dbus \
                    --net host \
                    ${env.DOCKER_IMAGE_TAG} \
                    bash -c 'cd /tmp/panda && PEDAL=1 scons && cp /tmp/panda/board/obj/pedal.bin.signed /tmp/pedal.bin.signed && \
                    PEDAL=1 PEDAL_USB=1 scons && cp /tmp/panda/board/obj/pedal.bin.signed /tmp/pedal_usb.bin.signed && \
                    python ./tests/pedal/test_pedal.py'"
            }
          }
        }
      }
    }
    stage('HITL tests') {
      steps {
        lock(resource: "pandas", inversePrecedence: true, quantity: 1) {
          timeout(time: 20, unit: 'MINUTES') {
            script {
              sh "docker run --rm --privileged \
                    --volume /dev/bus/usb:/dev/bus/usb \
                    --volume /var/run/dbus:/var/run/dbus \
                    --net host \
                    ${env.DOCKER_IMAGE_TAG} \
                    bash -c 'cd /tmp/panda && scons && ./tests/automated/test.sh'"
            }
          }
        }
      }
    }
  }
}
