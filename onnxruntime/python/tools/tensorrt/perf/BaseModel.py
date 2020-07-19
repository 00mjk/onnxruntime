import subprocess
import onnxruntime
import os
import sys

class BaseModel(object): 
    def __init__(self, model_name, providers):
        self.model_name_ = model_name 
        self.providers_ = providers
        self.model_path_ = None
        self.session_ = None
        self.session_options_ = onnxruntime.SessionOptions()
        self.onnx_zoo_test_data_dir_ = None
        self.test_data_num_ = 1
        self.inputs_ = None
        self.outputs_ = []
        self.validate_decimal_ = 4
        self.cleanup_files = []

    def get_model_name(self):
        return self.model_name_

    def get_session(self):
        return self.session_

    def get_onnx_zoo_test_data_dir(self):
        return self.onnx_zoo_test_data_dir_

    def get_outputs(self):
        return self.outputs_

    def set_inputs(self, inputs):
        self.inputs_ = inputs

    def get_decimal(self):
        return self.validate_decimal_

    def get_session_options(self):
        return self.session_options_

    def set_session_options(self, session_options):
        self.session_options_ = session_options 

    def get_input_name(self):
        if self.session_:
            return self.session_.get_inputs()
        return None

    def create_session(self, model_path=None):
        if not model_path:
            model_path = self.model_path_

        try: 
            self.session_ = onnxruntime.InferenceSession(model_path, providers=self.providers_, sess_options=self.session_options_)
            return
        except:
            print("Use symbolic_shape_infer.py")
        
        try:
            model_new_path = model_path[:].replace(".onnx", "_new.onnx")
            subprocess.run("python3 ../symbolic_shape_infer.py --input " + model_path + " --output " + model_new_path + " --auto_merge", shell=True, check=True)     
            self.session_ = onnxruntime.InferenceSession(model_new_path, providers=self.providers_, sess_options=self.session_options_)
            return
        except Exception as e:
            self.session_ = None
            print(e)
            raise
